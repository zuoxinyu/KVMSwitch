// gui.cpp : GTK3 托盘、快速切换窗口与设置窗口
#include "kvmswitch.h"

#ifdef HAVE_GTK

#include <gtk/gtk.h>
#include <glib-unix.h>

#ifdef HAVE_APPINDICATOR
#if defined(APPINDICATOR_AYATANA)
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif
#endif

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <sstream>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kvm {

namespace {

// ------------------------------------------------------------
// 全局 GUI 状态
// ------------------------------------------------------------

struct GuiState {
    std::string cfgPath;
    AppConfig cfg;
    MonitorContext ctx;
    GtkWidget* menu = nullptr;
    GtkWidget* settingsWindow = nullptr;
    GtkWidget* quickWindow = nullptr;
    int lockFd = -1;
    int listenFd = -1;
    guint socketSource = 0;
    bool quitting = false;

#ifdef HAVE_APPINDICATOR
    AppIndicator* indicator = nullptr;
#else
    GtkStatusIcon* statusIcon = nullptr;
#endif
};

struct SwitchAction {
    size_t monitorIndex = 0;
    uint16_t code = 0;
};

struct PresetAction {
    std::string name;
};

void deleteSwitchAction(gpointer data) { delete static_cast<SwitchAction*>(data); }
void deletePresetAction(gpointer data) { delete static_cast<PresetAction*>(data); }

std::string monitorCurrentName(const MonitorInfo& monitor, const AppConfig& cfg) {
    return monitor.currentInput ? getInputName(monitor.currentInput, &cfg) : "未知";
}

std::string trayTitle(const GuiState* state) {
    if (state->ctx.monitors.empty()) return "KVMSwitch - 未检测到显示器";
    std::string title = "KVMSwitch - ";
    for (size_t i = 0; i < state->ctx.monitors.size(); ++i) {
        if (i) title += ", ";
        title += "#" + std::to_string(i + 1) + ": " + monitorCurrentName(state->ctx.monitors[i], state->cfg);
    }
    return title;
}

std::string buildQueryText(const GuiState* state) {
    std::ostringstream out;
    out << "KVMSwitch 显示器检测\n";
    out << "发现 " << state->ctx.monitors.size() << " 台支持 DDC/CI 的外接显示器\n";
    for (const auto& monitor : state->ctx.monitors) {
        out << "\n[" << monitor.index1 << "] " << monitor.description;
        if (monitor.isPrimary) out << " (主显示器)";
        out << "\n    连接器: " << monitor.connector << "  (" << monitor.drmPath << ")\n";
        out << "    DDC 总线: " << monitor.i2cBus << "\n";
        out << "    当前输入: " << monitorCurrentName(monitor, state->cfg)
            << " (" << formatHexCode(monitor.currentInput) << ")\n";
        out << "    支持输入: ";
        const auto codes = getSupportedInputCodes(monitor, state->ctx.detectedTypeCCode);
        for (size_t i = 0; i < codes.size(); ++i) {
            if (i) out << ", ";
            out << getInputName(codes[i], &state->cfg) << " (" << formatHexCode(codes[i]) << ")";
        }
        out << "\n";
    }
    out << "\nType-C VCP 代码: " << formatHexCode(state->ctx.detectedTypeCCode) << "\n";
    return out.str();
}

void showMessage(GtkWindow* parent, GtkMessageType type, const std::string& title,
                 const std::string& message) {
    GtkWidget* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
                                              type, GTK_BUTTONS_OK, "%s", message.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void showTextWindow(GtkWindow* parent, const std::string& title, const std::string& text) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(title.c_str(), parent,
                                                    GTK_DIALOG_MODAL,
                                                    "_关闭", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 460);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget* view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroller), view);
    gtk_box_pack_start(GTK_BOX(area), scroller, TRUE, TRUE, 8);
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, text.c_str(), -1);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

std::string promptPresetName(GtkWindow* parent, const std::string& summary) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("保存多显示器预设", parent,
                                                    GTK_DIALOG_MODAL,
                                                    "_取消", GTK_RESPONSE_CANCEL,
                                                    "_保存", GTK_RESPONSE_OK, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, 240);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget* label = gtk_label_new("当前显示器输入状态将被保存为新预设：");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget* summaryLabel = gtk_label_new(summary.c_str());
    gtk_label_set_line_wrap(GTK_LABEL(summaryLabel), TRUE);
    gtk_widget_set_halign(summaryLabel, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), summaryLabel, FALSE, FALSE, 0);

    GtkWidget* nameLabel = gtk_label_new("预设名称 (例如: 双屏办公、游戏娱乐):");
    gtk_widget_set_halign(nameLabel, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), nameLabel, FALSE, FALSE, 0);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "我的预设");
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(area), box, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);

    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        result = trim(gtk_entry_get_text(GTK_ENTRY(entry)));
    }
    gtk_widget_destroy(dialog);
    return result;
}

void updateTrayTitle(GuiState* state) {
    const std::string title = trayTitle(state);
#ifdef HAVE_APPINDICATOR
    if (state->indicator) app_indicator_set_title(state->indicator, title.c_str());
#else
    if (state->statusIcon) gtk_status_icon_set_tooltip_text(state->statusIcon, title.c_str());
#endif
}

void refreshData(GuiState* state, bool reloadConfig = true) {
    const uint16_t typeCCode = state->ctx.detectedTypeCCode;
    if (reloadConfig) state->cfg = loadConfig(state->cfgPath, typeCCode);
    state->ctx = queryMonitors(state->cfg, true, typeCCode, true);
    updateTrayTitle(state);
}


// ------------------------------------------------------------
// 菜单
// ------------------------------------------------------------

void rebuildMenu(GuiState* state);

void onSwitchActivate(GtkMenuItem* item, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    auto* action = static_cast<SwitchAction*>(g_object_get_data(G_OBJECT(item), "kvm-switch-action"));
    if (!action || action->monitorIndex >= state->ctx.monitors.size()) return;

    auto& monitor = state->ctx.monitors[action->monitorIndex];
    std::string error;
    if (switchMonitor(monitor, action->code, &error)) {
        monitor.currentInput = action->code;
        if (auto it = state->cfg.commands.find(action->code); it != state->cfg.commands.end()) {
            executeShellCommand(it->second);
        }
        if (state->cfg.notify) {
            notifyDesktop("KVMSwitch 输入切换",
                          monitor.description + " 已切换至: " + getInputName(action->code, &state->cfg));
        }
    } else {
        showMessage(nullptr, GTK_MESSAGE_ERROR, "KVMSwitch 错误",
                    "切换显示器输入源失败：\n" + error +
                    "\n\n请检查显示器 DDC/CI 设置、线缆连接与 /dev/i2c-* 权限。");
    }
    refreshData(state);
    rebuildMenu(state);
}

void onPresetActivate(GtkMenuItem* item, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    auto* action = static_cast<PresetAction*>(g_object_get_data(G_OBJECT(item), "kvm-preset-action"));
    if (!action) return;

    const MonitorPreset* preset = nullptr;
    for (const auto& candidate : state->cfg.presets) {
        if (candidate.name == action->name) {
            preset = &candidate;
            break;
        }
    }
    if (!preset) {
        showMessage(nullptr, GTK_MESSAGE_WARNING, "KVMSwitch", "预设不存在，配置可能已被修改。");
        refreshData(state);
        rebuildMenu(state);
        return;
    }

    std::string summary;
    applyPreset(state->cfg, state->ctx, *preset, state->ctx.detectedTypeCCode, &summary);
    refreshData(state);
    rebuildMenu(state);
}

void onToggleActivate(GtkMenuItem*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    std::string error;
    if (!toggleInput(state->cfg, state->ctx, state->ctx.detectedTypeCCode, &error)) {
        showMessage(nullptr, GTK_MESSAGE_ERROR, "KVMSwitch 错误", "轮换切换失败：\n" + error);
    }
    refreshData(state);
    rebuildMenu(state);
}

void onRefreshActivate(GtkMenuItem*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    refreshData(state);
    rebuildMenu(state);
}

void onSavePresetActivate(GtkMenuItem*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    if (state->ctx.monitors.empty()) {
        showMessage(nullptr, GTK_MESSAGE_WARNING, "KVMSwitch", "未检测到外接显示器，无法保存预设。");
        return;
    }

    refreshMonitorStates(state->ctx);
    std::string summary;
    for (const auto& monitor : state->ctx.monitors) {
        if (!summary.empty()) summary += "\n";
        summary += "• #" + std::to_string(monitor.index1) + " " + monitor.description +
                   ": " + monitorCurrentName(monitor, state->cfg);
    }

    const std::string name = promptPresetName(nullptr, summary);
    if (name.empty()) return;

    std::string error;
    if (!saveCurrentStateAsPreset(state->cfgPath, state->ctx, name,
                                  state->ctx.detectedTypeCCode, &error)) {
        showMessage(nullptr, GTK_MESSAGE_ERROR, "KVMSwitch 错误", "保存预设失败：\n" + error);
        return;
    }

    refreshData(state);
    rebuildMenu(state);
    notifyDesktop("KVMSwitch 预设已保存", "预设「" + name + "」保存成功。");
}

void onAutostartActivate(GtkCheckMenuItem* item, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    const bool enable = gtk_check_menu_item_get_active(item) != FALSE;
    std::string error;
    if (!setAutostart(enable, &error)) {
        showMessage(nullptr, GTK_MESSAGE_ERROR, "KVMSwitch 错误", "修改自启动设置失败：\n" + error);
    } else {
        notifyDesktop("KVMSwitch 开机自启动",
                      enable ? "已开启开机自启动 (常驻托盘)。" : "已关闭开机自启动。");
    }
    rebuildMenu(state);
}

void onCreateShortcutsActivate(GtkMenuItem*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    std::string message;
    const bool ok = createDesktopShortcuts(state->cfg, &message);
    showMessage(nullptr, ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING,
                ok ? "快捷方式创建成功" : "快捷方式创建失败", message);
}

void onOpenConfigActivate(GtkMenuItem*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    ensureConfigFile(state->cfgPath);
    openPathWithDesktop(state->cfgPath);
}

void onQueryActivate(GtkMenuItem*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    refreshData(state);
    showTextWindow(nullptr, "KVMSwitch 显示器信息", buildQueryText(state));
}

void showSettingsWindow(GuiState* state);

void onSettingsActivate(GtkMenuItem*, gpointer data) {
    showSettingsWindow(static_cast<GuiState*>(data));
}

void onQuitActivate(GtkMenuItem*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    state->quitting = true;
    gtk_main_quit();
}

GtkWidget* makeDisabledItem(const std::string& text) {
    GtkWidget* item = gtk_menu_item_new_with_label(text.c_str());
    gtk_widget_set_sensitive(item, FALSE);
    return item;
}

GtkWidget* makeSwitchItem(const MonitorInfo& monitor, size_t monitorIndex, uint16_t code,
                          const AppConfig& cfg, GuiState* state) {
    const std::string label = "   " + getInputName(code, &cfg) + "  (" +
                              std::string("0x") + [&] {
                                  char buffer[8];
                                  std::snprintf(buffer, sizeof(buffer), "%02X", code);
                                  return std::string(buffer);
                              }() + ")";
    GtkWidget* item = gtk_check_menu_item_new_with_label(label.c_str());
    gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), code == monitor.currentInput);
    auto* action = new SwitchAction{monitorIndex, code};
    g_object_set_data_full(G_OBJECT(item), "kvm-switch-action", action, deleteSwitchAction);
    g_signal_connect(item, "activate", G_CALLBACK(onSwitchActivate), state);
    return item;
}

void rebuildMenu(GuiState* state) {
    if (!state->menu) return;

    GList* children = gtk_container_get_children(GTK_CONTAINER(state->menu));
    for (GList* node = children; node; node = node->next) gtk_widget_destroy(GTK_WIDGET(node->data));
    g_list_free(children);

    // 1. 预设方案
    if (!state->cfg.presets.empty()) {
        gtk_menu_shell_append(GTK_MENU_SHELL(state->menu),
                              makeDisabledItem("📐 多显示器预设方案"));
        for (const auto& preset : state->cfg.presets) {
            GtkWidget* item = gtk_menu_item_new_with_label(("   ▶ " + preset.name).c_str());
            auto* action = new PresetAction{preset.name};
            g_object_set_data_full(G_OBJECT(item), "kvm-preset-action", action, deletePresetAction);
            g_signal_connect(item, "activate", G_CALLBACK(onPresetActivate), state);
            gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), item);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), gtk_separator_menu_item_new());
    }

    // 2. 轮换
    GtkWidget* toggleItem = gtk_menu_item_new_with_label("🔄 轮流切换输入源 (Toggle)");
    g_signal_connect(toggleItem, "activate", G_CALLBACK(onToggleActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), toggleItem);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), gtk_separator_menu_item_new());

    // 3. 显示器与输入源
    if (state->ctx.monitors.empty()) {
        gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), makeDisabledItem("未检测到外接显示器"));
    } else if (state->ctx.monitors.size() == 1) {
        const auto& monitor = state->ctx.monitors.front();
        gtk_menu_shell_append(GTK_MENU_SHELL(state->menu),
                              makeDisabledItem("🖥️ " + monitor.description + " [当前: " +
                                               monitorCurrentName(monitor, state->cfg) + "]"));
        for (uint16_t code : getSupportedInputCodes(monitor, state->ctx.detectedTypeCCode)) {
            gtk_menu_shell_append(GTK_MENU_SHELL(state->menu),
                                  makeSwitchItem(monitor, 0, code, state->cfg, state));
        }
    } else {
        for (size_t i = 0; i < state->ctx.monitors.size(); ++i) {
            const auto& monitor = state->ctx.monitors[i];
            std::string title = "🖥️ #" + std::to_string(i + 1) + " " + monitor.description;
            if (monitor.isPrimary) title += " (主)";
            title += "  [" + monitorCurrentName(monitor, state->cfg) + "]";
            GtkWidget* monItem = gtk_menu_item_new_with_label(title.c_str());
            GtkWidget* submenu = gtk_menu_new();
            for (uint16_t code : getSupportedInputCodes(monitor, state->ctx.detectedTypeCCode)) {
                gtk_menu_shell_append(GTK_MENU_SHELL(submenu),
                                      makeSwitchItem(monitor, i, code, state->cfg, state));
            }
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(monItem), submenu);
            gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), monItem);
        }
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), gtk_separator_menu_item_new());

    // 4. 工具与设置
    GtkWidget* saveItem = gtk_menu_item_new_with_label("💾 保存当前状态为多显示器预设...");
    g_signal_connect(saveItem, "activate", G_CALLBACK(onSavePresetActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), saveItem);

    GtkWidget* refreshItem = gtk_menu_item_new_with_label("🔄 刷新显示器状态");
    g_signal_connect(refreshItem, "activate", G_CALLBACK(onRefreshActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), refreshItem);

    GtkWidget* settingsItem = gtk_menu_item_new_with_label("⚙️ 设置...");
    g_signal_connect(settingsItem, "activate", G_CALLBACK(onSettingsActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), settingsItem);

    GtkWidget* shortcutsItem = gtk_menu_item_new_with_label("🔗 创建桌面快捷方式 (一键切换)");
    g_signal_connect(shortcutsItem, "activate", G_CALLBACK(onCreateShortcutsActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), shortcutsItem);

    GtkWidget* configItem = gtk_menu_item_new_with_label("📄 打开配置文件 (config.ini)");
    g_signal_connect(configItem, "activate", G_CALLBACK(onOpenConfigActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), configItem);

    GtkWidget* queryItem = gtk_menu_item_new_with_label("🔍 检测显示器信息与输入状态");
    g_signal_connect(queryItem, "activate", G_CALLBACK(onQueryActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), queryItem);

    GtkWidget* autostartItem = gtk_check_menu_item_new_with_label("🚀 开机自启动 (常驻系统托盘)");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(autostartItem), isAutostartEnabled());
    g_signal_connect(autostartItem, "activate", G_CALLBACK(onAutostartActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), autostartItem);

    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), gtk_separator_menu_item_new());

    GtkWidget* quitItem = gtk_menu_item_new_with_label("✕ 退出");
    g_signal_connect(quitItem, "activate", G_CALLBACK(onQuitActivate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), quitItem);

    gtk_widget_show_all(state->menu);
}


// ------------------------------------------------------------
// 托盘 / 单实例 IPC
// ------------------------------------------------------------

gboolean rebuildMenuIdle(gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    if (!state->quitting) rebuildMenu(state);
    return G_SOURCE_REMOVE;
}

void onMenuShow(GtkWidget*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    if (state->quitting) return;
    refreshData(state);
    g_idle_add(rebuildMenuIdle, state);
}

gboolean onTimerTick(gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    if (!state->quitting) {
        refreshData(state);
        rebuildMenu(state);
    }
    return G_SOURCE_CONTINUE;
}

#ifndef HAVE_APPINDICATOR
void onStatusIconPopup(GtkStatusIcon* icon, guint button, guint time, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    if (!state->menu) return;
    refreshData(state);
    rebuildMenu(state);
    gtk_menu_popup(GTK_MENU(state->menu), nullptr, nullptr,
                   gtk_status_icon_position_menu, state->statusIcon, button, time);
}
#endif

void cleanupTray(GuiState* state) {
    if (state->socketSource) {
        g_source_remove(state->socketSource);
        state->socketSource = 0;
    }
    if (state->listenFd >= 0) {
        ::close(state->listenFd);
        state->listenFd = -1;
    }
    const std::string socketPath = traySocketPath();
    ::unlink(socketPath.c_str());

    if (state->menu) {
        gtk_widget_destroy(state->menu);
        state->menu = nullptr;
    }

#ifdef HAVE_APPINDICATOR
    if (state->indicator) {
        app_indicator_set_status(state->indicator, APP_INDICATOR_STATUS_PASSIVE);
        g_object_unref(state->indicator);
        state->indicator = nullptr;
    }
#else
    if (state->statusIcon) {
        gtk_status_icon_set_visible(state->statusIcon, FALSE);
        g_object_unref(state->statusIcon);
        state->statusIcon = nullptr;
    }
#endif
    if (state->lockFd >= 0) {
        ::flock(state->lockFd, LOCK_UN);
        ::close(state->lockFd);
        state->lockFd = -1;
    }
}

GtkWidget* createQuickWindow(GuiState* state, bool standalone);

void handleTrayCommand(GuiState* state, const std::string& command) {
    if (command == "ping") return;

    if (command == "show-settings") {
        showSettingsWindow(state);
    } else if (command == "show-menu") {
        // Wayland 下无法可靠地在全局指针位置弹出 GtkMenu，
        // 因此二次启动 / --menu 统一显示轻量快速切换窗口。
        refreshData(state);
        if (state->quickWindow) {
            gtk_window_present(GTK_WINDOW(state->quickWindow));
        } else {
            GtkWidget* window = createQuickWindow(state, false);
            gtk_widget_show_all(window);
        }
    } else if (command == "refresh") {
        refreshData(state);
        rebuildMenu(state);
    } else if (command == "quit") {
        state->quitting = true;
        gtk_main_quit();
    } else if (startsWith(command, "switch:")) {
        const uint16_t code = static_cast<uint16_t>(std::strtoul(command.c_str() + 7, nullptr, 0));
        if (code > 0) {
            std::string error;
            if (switchTargetMonitors(state->cfg, state->ctx, code, &error) == 0) {
                notifyDesktop("KVMSwitch 错误", "切换输入源失败: " + error);
            }
            refreshData(state);
            rebuildMenu(state);
        }
    } else if (startsWith(command, "preset:")) {
        const std::string name = command.substr(7);
        for (const auto& preset : state->cfg.presets) {
            if (preset.name == name) {
                std::string summary;
                applyPreset(state->cfg, state->ctx, preset, state->ctx.detectedTypeCCode, &summary);
                refreshData(state);
                rebuildMenu(state);
                break;
            }
        }
    }
}

gboolean onSocketEvent(gint fd, GIOCondition condition, gpointer data) {
    if (!(condition & G_IO_IN)) return G_SOURCE_CONTINUE;
    auto* state = static_cast<GuiState*>(data);

    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) return G_SOURCE_CONTINUE;

    char buffer[512] = {0};
    const ssize_t n = ::read(client, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        const std::string command = trim(std::string(buffer, static_cast<size_t>(n)));
        handleTrayCommand(state, command);
        if (command == "ping") {
            const char* ok = "ok\n";
            (void)!::write(client, ok, 3);
        }
    }
    ::close(client);
    return G_SOURCE_CONTINUE;
}

gboolean onTerminateSignal(gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    state->quitting = true;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

bool setupTraySocket(GuiState* state) {
    const std::string path = traySocketPath();
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return false;

    ::unlink(path.c_str());
    state->listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (state->listenFd < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::bind(state->listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(state->listenFd);
        state->listenFd = -1;
        return false;
    }
    ::chmod(path.c_str(), 0600);
    if (::listen(state->listenFd, 8) != 0) {
        ::close(state->listenFd);
        state->listenFd = -1;
        return false;
    }
    state->socketSource = g_unix_fd_add(state->listenFd, G_IO_IN, onSocketEvent, state);
    return true;
}

} // namespace

// ------------------------------------------------------------
// 托盘模式入口
// ------------------------------------------------------------

int runTrayApp(const std::string& cfgPath, AppConfig cfg, MonitorContext ctx) {
    auto state = std::make_unique<GuiState>();
    state->cfgPath = cfgPath;
    state->cfg = std::move(cfg);
    state->ctx = std::move(ctx);

    // 单实例锁：如果已有托盘实例，转发 show-menu 后立即退出
    const std::string lockPath = (fs::path(getRuntimeDir()) / "kvmswitch.lock").string();
    state->lockFd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (state->lockFd < 0 || ::flock(state->lockFd, LOCK_EX | LOCK_NB) != 0) {
        if (state->lockFd >= 0) {
            ::close(state->lockFd);
            state->lockFd = -1;
        }
        sendTrayCommand("show-menu");
        return 0;
    }

    if (!gtk_init_check(nullptr, nullptr)) {
        std::fprintf(stderr, "无法初始化 GTK：请确认已安装 GTK3 并在图形桌面会话中运行。\n");
        return 1;
    }

    refreshData(state.get(), true);

    state->menu = gtk_menu_new();
    g_signal_connect(state->menu, "show", G_CALLBACK(onMenuShow), state.get());
    rebuildMenu(state.get());

    const std::string iconPath = getIconPath();
#ifdef HAVE_APPINDICATOR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    state->indicator = app_indicator_new("kvmswitch",
                                         iconPath.empty() ? "video-display" : "kvmswitch",
                                         APP_INDICATOR_CATEGORY_HARDWARE);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    if (state->indicator) {
        if (!iconPath.empty()) {
            const fs::path iconFs(iconPath);
            app_indicator_set_icon_theme_path(state->indicator, iconFs.parent_path().c_str());
            app_indicator_set_icon_full(state->indicator, iconFs.stem().c_str(), "KVMSwitch");
        }
        app_indicator_set_status(state->indicator, APP_INDICATOR_STATUS_ACTIVE);
        app_indicator_set_title(state->indicator, trayTitle(state.get()).c_str());
        app_indicator_set_menu(state->indicator, GTK_MENU(state->menu));
    } else {
        std::fprintf(stderr, "无法创建 AppIndicator，托盘可能不可用。\n");
    }
#else
    if (!iconPath.empty()) state->statusIcon = gtk_status_icon_new_from_file(iconPath.c_str());
    else state->statusIcon = gtk_status_icon_new_from_icon_name("video-display");
    gtk_status_icon_set_title(state->statusIcon, "KVMSwitch");
    gtk_status_icon_set_tooltip_text(state->statusIcon, trayTitle(state.get()).c_str());
    g_signal_connect(state->statusIcon, "popup-menu", G_CALLBACK(onStatusIconPopup), state.get());
    gtk_status_icon_set_visible(state->statusIcon, TRUE);
#endif

    setupTraySocket(state.get());
    g_unix_signal_add(SIGTERM, onTerminateSignal, state.get());
    g_unix_signal_add(SIGINT, onTerminateSignal, state.get());
    g_timeout_add_seconds(15, onTimerTick, state.get());

    gtk_main();
    cleanupTray(state.get());
    return 0;
}


// ------------------------------------------------------------
// 设置窗口
// ------------------------------------------------------------

namespace {

struct InputChoice {
    int monitorIndex = 0;
    std::vector<uint16_t> codes;
};

struct SettingsUi {
    GuiState* app = nullptr;
    AppConfig draft;
    MonitorContext monitors;
    bool standalone = false;
    bool loading = false;

    GtkWidget* window = nullptr;
    GtkListStore* presetStore = nullptr;
    GtkTreeView* presetView = nullptr;
    GtkWidget* nameEntry = nullptr;
    GtkWidget* monitorBox = nullptr;
    GtkWidget* statusLabel = nullptr;
    std::vector<GtkWidget*> combos;

    GtkWidget* notifyCheck = nullptr;
    GtkWidget* autostartCheck = nullptr;
    GtkWidget* modeCombo = nullptr;
    GtkWidget* targetCombo = nullptr;
    GtkWidget* toggleEntry = nullptr;
    GtkWidget* cmdEntries[4] = {nullptr, nullptr, nullptr, nullptr};
    uint16_t cmdCodes[4] = {16, 15, 17, 18};
};

void deleteInputChoice(gpointer data) { delete static_cast<InputChoice*>(data); }

enum {
    PRESET_COL_NAME = 0,
    PRESET_COL_INDEX,
    PRESET_COL_COUNT
};

int selectedPresetIndex(SettingsUi* ui) {
    if (!ui->presetView) return -1;
    GtkTreeSelection* selection = gtk_tree_view_get_selection(ui->presetView);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return -1;
    gint index = -1;
    gtk_tree_model_get(model, &iter, PRESET_COL_INDEX, &index, -1);
    return index;
}

void updateStoreName(SettingsUi* ui, int index, const std::string& name) {
    if (!ui->presetStore || index < 0) return;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->presetStore), &iter);
    int current = 0;
    while (valid) {
        if (current == index) {
            gtk_list_store_set(ui->presetStore, &iter, PRESET_COL_NAME, name.c_str(), -1);
            return;
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->presetStore), &iter);
        ++current;
    }
}

void loadPresetEditor(SettingsUi* ui) {
    const int index = selectedPresetIndex(ui);
    const bool enabled = index >= 0 && index < static_cast<int>(ui->draft.presets.size());
    ui->loading = true;

    gtk_widget_set_sensitive(ui->nameEntry, enabled);
    for (GtkWidget* combo : ui->combos) gtk_widget_set_sensitive(combo, enabled);

    if (enabled) {
        const MonitorPreset& preset = ui->draft.presets[static_cast<size_t>(index)];
        gtk_entry_set_text(GTK_ENTRY(ui->nameEntry), preset.name.c_str());
        for (size_t i = 0; i < ui->combos.size() && i < ui->monitors.monitors.size(); ++i) {
            auto* choice = static_cast<InputChoice*>(
                g_object_get_data(G_OBJECT(ui->combos[i]), "kvm-input-choice"));
            if (!choice) continue;
            uint16_t code = 0;
            for (const auto& target : preset.targets) {
                if (matchMonitor(ui->monitors.monitors[i], target.monitorId)) {
                    code = target.inputCode;
                    break;
                }
            }
            int active = 0;
            for (size_t j = 0; j < choice->codes.size(); ++j) {
                if (choice->codes[j] == code) {
                    active = static_cast<int>(j + 1);
                    break;
                }
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(ui->combos[i]), active);
        }
        if (ui->statusLabel) {
            gtk_label_set_text(GTK_LABEL(ui->statusLabel),
                               "修改后点击“保存”写入 config.ini，并立即同步到托盘。");
        }
    } else {
        gtk_entry_set_text(GTK_ENTRY(ui->nameEntry), "");
        for (GtkWidget* combo : ui->combos) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
        if (ui->statusLabel) {
            gtk_label_set_text(GTK_LABEL(ui->statusLabel), "从左侧选择预设，或点击“新建”创建方案。");
        }
    }
    ui->loading = false;
}

void refreshPresetStore(SettingsUi* ui, int selectIndex) {
    if (!ui->presetStore) return;
    ui->loading = true;
    gtk_list_store_clear(ui->presetStore);
    for (size_t i = 0; i < ui->draft.presets.size(); ++i) {
        GtkTreeIter iter;
        gtk_list_store_append(ui->presetStore, &iter);
        gtk_list_store_set(ui->presetStore, &iter,
                           PRESET_COL_NAME, ui->draft.presets[i].name.c_str(),
                           PRESET_COL_INDEX, static_cast<int>(i), -1);
    }
    GtkTreeSelection* selection = gtk_tree_view_get_selection(ui->presetView);
    if (selectIndex >= 0 && selectIndex < static_cast<int>(ui->draft.presets.size())) {
        GtkTreeIter iter;
        if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ui->presetStore), &iter, nullptr, selectIndex)) {
            gtk_tree_selection_select_iter(selection, &iter);
        }
    } else {
        gtk_tree_selection_unselect_all(selection);
    }
    ui->loading = false;
    loadPresetEditor(ui);
}

void onPresetSelectionChanged(GtkTreeSelection*, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    if (!ui->loading) loadPresetEditor(ui);
}

void onPresetNameChanged(GtkEditable* editable, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    if (ui->loading) return;
    const int index = selectedPresetIndex(ui);
    if (index < 0 || index >= static_cast<int>(ui->draft.presets.size())) return;
    const std::string name = gtk_entry_get_text(GTK_ENTRY(editable));
    ui->draft.presets[static_cast<size_t>(index)].name = name;
    updateStoreName(ui, index, name);
}

void updatePresetTarget(SettingsUi* ui, int index, int monitorIndex, uint16_t code) {
    if (index < 0 || index >= static_cast<int>(ui->draft.presets.size())) return;
    if (monitorIndex < 0 || monitorIndex >= static_cast<int>(ui->monitors.monitors.size())) return;

    const MonitorInfo& monitor = ui->monitors.monitors[static_cast<size_t>(monitorIndex)];
    auto& targets = ui->draft.presets[static_cast<size_t>(index)].targets;

    for (auto it = targets.begin(); it != targets.end(); ++it) {
        if (matchMonitor(monitor, it->monitorId)) {
            if (code == 0) {
                targets.erase(it);
            } else {
                it->inputCode = code;
            }
            return;
        }
    }
    if (code > 0) targets.push_back({std::to_string(monitor.index1), code});
}

void onPresetInputChanged(GtkComboBox* combo, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    if (ui->loading) return;
    const int index = selectedPresetIndex(ui);
    if (index < 0) return;

    auto* choice = static_cast<InputChoice*>(g_object_get_data(G_OBJECT(combo), "kvm-input-choice"));
    if (!choice) return;
    const int active = gtk_combo_box_get_active(combo);
    uint16_t code = 0;
    if (active > 0 && static_cast<size_t>(active - 1) < choice->codes.size()) {
        code = choice->codes[static_cast<size_t>(active - 1)];
    }
    updatePresetTarget(ui, index, choice->monitorIndex, code);
}

std::string uniquePresetName(const AppConfig& cfg) {
    const std::string base = "新建预设";
    std::string name = base;
    int suffix = 2;
    auto exists = [&](const std::string& candidate) {
        for (const auto& preset : cfg.presets) {
            if (toUpper(preset.name) == toUpper(candidate)) return true;
        }
        return false;
    };
    while (exists(name)) name = base + " " + std::to_string(suffix++);
    return name;
}

void onPresetAdd(GtkButton*, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    MonitorPreset preset;
    preset.name = uniquePresetName(ui->draft);
    ui->draft.presets.push_back(preset);
    refreshPresetStore(ui, static_cast<int>(ui->draft.presets.size()) - 1);
}

void onPresetDelete(GtkButton*, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    const int index = selectedPresetIndex(ui);
    if (index < 0) return;
    ui->draft.presets.erase(ui->draft.presets.begin() + index);
    const int select = std::min(index, static_cast<int>(ui->draft.presets.size()) - 1);
    refreshPresetStore(ui, select);
}

void onPresetMove(GtkButton* button, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    const int index = selectedPresetIndex(ui);
    if (index < 0) return;
    const int direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "kvm-move-direction"));
    const int target = index + direction;
    if (target < 0 || target >= static_cast<int>(ui->draft.presets.size())) return;
    std::swap(ui->draft.presets[static_cast<size_t>(index)], ui->draft.presets[static_cast<size_t>(target)]);
    refreshPresetStore(ui, target);
}

GtkWidget* labeledRow(const char* label, GtkWidget* widget) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* labelWidget = gtk_label_new(label);
    gtk_widget_set_size_request(labelWidget, 170, -1);
    gtk_widget_set_halign(labelWidget, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(box), labelWidget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
    return box;
}

void buildPresetTab(SettingsUi* ui, GtkWidget* notebook) {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(page), paned, TRUE, TRUE, 0);

    // 左侧预设列表
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    ui->presetStore = gtk_list_store_new(PRESET_COL_COUNT, G_TYPE_STRING, G_TYPE_INT);
    ui->presetView = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->presetStore)));
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes("预设", renderer, "text",
                                                                        PRESET_COL_NAME, nullptr);
    gtk_tree_view_append_column(ui->presetView, column);
    gtk_tree_view_set_headers_visible(ui->presetView, FALSE);
    gtk_container_add(GTK_CONTAINER(scroller), GTK_WIDGET(ui->presetView));
    gtk_box_pack_start(GTK_BOX(left), scroller, TRUE, TRUE, 0);

    GtkWidget* buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_START);
    gtk_box_set_spacing(GTK_BOX(buttons), 6);
    GtkWidget* addButton = gtk_button_new_with_label("＋ 新建");
    GtkWidget* delButton = gtk_button_new_with_label("－ 删除");
    GtkWidget* upButton = gtk_button_new_with_label("↑ 上移");
    GtkWidget* downButton = gtk_button_new_with_label("↓ 下移");
    gtk_box_pack_start(GTK_BOX(buttons), addButton, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), delButton, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), upButton, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), downButton, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), buttons, FALSE, FALSE, 8);
    gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);
    gtk_widget_set_size_request(left, 210, -1);

    // 右侧编辑器
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(right), 16);

    ui->nameEntry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(right), labeledRow("预设名称:", ui->nameEntry), FALSE, FALSE, 0);

    GtkWidget* header = gtk_label_new("显示器目标输入源配置:");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right), header, FALSE, FALSE, 0);

    GtkWidget* monitorScroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(monitorScroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    ui->monitorBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(monitorScroller), ui->monitorBox);
    gtk_box_pack_start(GTK_BOX(right), monitorScroller, TRUE, TRUE, 0);

    if (ui->monitors.monitors.empty()) {
        GtkWidget* empty = gtk_label_new("未检测到外接显示器（请先连接显示器并刷新）");
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(ui->monitorBox), empty, FALSE, FALSE, 0);
    } else {
        for (size_t i = 0; i < ui->monitors.monitors.size(); ++i) {
            const auto& monitor = ui->monitors.monitors[i];
            GtkWidget* combo = gtk_combo_box_text_new();
            auto* choice = new InputChoice{static_cast<int>(i), {}};
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "不改变 (Don't Change)");
            for (uint16_t code : getSupportedInputCodes(monitor, ui->monitors.detectedTypeCCode)) {
                char hex[16];
                std::snprintf(hex, sizeof(hex), " (0x%02X)", code);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                               (getInputName(code, &ui->draft) + hex).c_str());
                choice->codes.push_back(code);
            }
            g_object_set_data_full(G_OBJECT(combo), "kvm-input-choice", choice, deleteInputChoice);
            g_signal_connect(combo, "changed", G_CALLBACK(onPresetInputChanged), ui);
            std::string label = "#" + std::to_string(monitor.index1) + " " + monitor.description +
                                " (" + monitor.connector + ")";
            if (monitor.isPrimary) label += " [主]";
            GtkWidget* row = labeledRow(label.c_str(), combo);
            gtk_box_pack_start(GTK_BOX(ui->monitorBox), row, FALSE, FALSE, 0);
            ui->combos.push_back(combo);
        }
    }

    ui->statusLabel = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(ui->statusLabel), TRUE);
    gtk_widget_set_halign(ui->statusLabel, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right), ui->statusLabel, FALSE, FALSE, 0);

    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new("📐 预设方案"));

    GtkTreeSelection* selection = gtk_tree_view_get_selection(ui->presetView);
    g_signal_connect(selection, "changed", G_CALLBACK(onPresetSelectionChanged), ui);
    g_signal_connect(ui->nameEntry, "changed", G_CALLBACK(onPresetNameChanged), ui);

    g_signal_connect(addButton, "clicked", G_CALLBACK(onPresetAdd), ui);
    g_signal_connect(delButton, "clicked", G_CALLBACK(onPresetDelete), ui);

    g_object_set_data(G_OBJECT(upButton), "kvm-move-direction", GINT_TO_POINTER(-1));
    g_signal_connect(upButton, "clicked", G_CALLBACK(onPresetMove), ui);
    g_object_set_data(G_OBJECT(downButton), "kvm-move-direction", GINT_TO_POINTER(1));
    g_signal_connect(downButton, "clicked", G_CALLBACK(onPresetMove), ui);

    refreshPresetStore(ui, ui->draft.presets.empty() ? -1 : 0);
}


uint16_t codeForAlias(const AppConfig& cfg, const std::vector<std::string>& aliases, uint16_t fallback) {
    for (const auto& kv : cfg.customInputs) {
        for (const auto& alias : aliases) {
            if (toUpper(kv.first) == alias) return kv.second;
        }
    }
    return fallback;
}

std::string toggleInputsText(const AppConfig& cfg, uint16_t detectedTypeCCode) {
    std::string text;
    for (size_t i = 0; i < cfg.toggleInputs.size(); ++i) {
        if (i) text += ", ";
        text += getInputAliasForCode(cfg.toggleInputs[i], detectedTypeCCode);
    }
    return text;
}

void buildGeneralTab(SettingsUi* ui, GtkWidget* notebook) {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(page), 18);

    ui->notifyCheck = gtk_check_button_new_with_label("切换输入源成功后弹出桌面通知");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->notifyCheck), ui->draft.notify);
    gtk_box_pack_start(GTK_BOX(page), ui->notifyCheck, FALSE, FALSE, 0);

    ui->autostartCheck = gtk_check_button_new_with_label("开机自启动 (登录桌面后常驻系统托盘)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->autostartCheck), isAutostartEnabled());
    gtk_box_pack_start(GTK_BOX(page), ui->autostartCheck, FALSE, FALSE, 0);

    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(page), separator, FALSE, FALSE, 2);

    ui->modeCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->modeCombo), "tray — 启动后常驻系统托盘");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->modeCombo), "menu — 弹出快速切换窗口");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->modeCombo), "toggle — 一键轮换输入源");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->modeCombo),
                             ui->draft.mode == "menu" ? 1 : (ui->draft.mode == "toggle" ? 2 : 0));
    gtk_box_pack_start(GTK_BOX(page), labeledRow("运行模式:", ui->modeCombo), FALSE, FALSE, 0);

    ui->targetCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->targetCombo), "primary — 仅主显示器");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->targetCombo), "all — 所有显示器");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->targetCombo),
                             ui->draft.targetMonitor == "all" ? 1 : 0);
    gtk_box_pack_start(GTK_BOX(page), labeledRow("目标显示器:", ui->targetCombo), FALSE, FALSE, 0);

    ui->toggleEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ui->toggleEntry),
                       toggleInputsText(ui->draft, ui->monitors.detectedTypeCCode).c_str());
    gtk_box_pack_start(GTK_BOX(page), labeledRow("Toggle 轮换序列:", ui->toggleEntry), FALSE, FALSE, 0);

    GtkWidget* cmdHeader = gtk_label_new("联动命令 (切换到对应输入源后通过 /bin/sh -c 在后台执行，留空则不执行):");
    gtk_widget_set_halign(cmdHeader, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), cmdHeader, FALSE, FALSE, 4);

    ui->cmdCodes[0] = codeForAlias(ui->draft, {"DP", "DP1"}, 16);
    ui->cmdCodes[1] = codeForAlias(ui->draft, {"TYPEC", "TYPE-C", "USBC", "USB-C"},
                                   ui->monitors.detectedTypeCCode);
    ui->cmdCodes[2] = 17;
    ui->cmdCodes[3] = 18;
    const char* labels[4] = {"DisplayPort 命令:", "USB Type-C 命令:", "HDMI 1 命令:", "HDMI 2 命令:"};
    for (int i = 0; i < 4; ++i) {
        char label[128];
        std::snprintf(label, sizeof(label), "%s (0x%02X)", labels[i], ui->cmdCodes[i]);
        ui->cmdEntries[i] = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(ui->cmdEntries[i]), "例如: ssh mac caffeinate -u -t 2");
        auto it = ui->draft.commands.find(ui->cmdCodes[i]);
        if (it != ui->draft.commands.end()) {
            gtk_entry_set_text(GTK_ENTRY(ui->cmdEntries[i]), it->second.c_str());
        }
        gtk_box_pack_start(GTK_BOX(page), labeledRow(label, ui->cmdEntries[i]), FALSE, FALSE, 0);
    }

    GtkWidget* note = gtk_label_new("提示：模式修改在下次启动或重新登录后生效；托盘当前实例会继续使用原模式。\n"
                                    "预设方案会同时写入 config.ini 的 [Presets] 节，与 Windows / macOS 端格式兼容。");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 4);

    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), page);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroller, gtk_label_new("⚙️ 通用偏好与联动"));
}

bool validatePresets(const AppConfig& cfg, std::string* error) {
    std::vector<std::string> names;
    for (const auto& preset : cfg.presets) {
        const std::string name = trim(preset.name);
        if (name.empty()) {
            if (error) *error = "预设名称不能为空。";
            return false;
        }
        for (const auto& existing : names) {
            if (toUpper(existing) == toUpper(name)) {
                if (error) *error = "预设名称「" + name + "」重复，请修改后再保存。";
                return false;
            }
        }
        names.push_back(name);
    }
    return true;
}

void deleteSettingsUi(gpointer data) { delete static_cast<SettingsUi*>(data); }

void onSettingsDestroy(GtkWidget* widget, gpointer) {
    auto* ui = static_cast<SettingsUi*>(g_object_get_data(G_OBJECT(widget), "settings-ui"));
    if (!ui) return;
    GuiState* state = ui->app;
    if (state && state->settingsWindow == widget) state->settingsWindow = nullptr;
    if (ui->standalone) gtk_main_quit();
}

void onSettingsSave(GtkButton*, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    AppConfig& draft = ui->draft;

    // 从通用页读取控件值
    draft.notify = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->notifyCheck)) != FALSE;
    const int modeIndex = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->modeCombo));
    draft.mode = modeIndex == 1 ? "menu" : (modeIndex == 2 ? "toggle" : "tray");
    draft.targetMonitor = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->targetCombo)) == 1 ? "all" : "primary";

    const std::string toggleText = gtk_entry_get_text(GTK_ENTRY(ui->toggleEntry));
    std::vector<uint16_t> toggleInputs;
    for (const auto& item : split(toggleText, ',')) {
        const uint16_t code = parseInputAlias(item, draft, ui->monitors.detectedTypeCCode);
        if (code > 0 && std::find(toggleInputs.begin(), toggleInputs.end(), code) == toggleInputs.end()) {
            toggleInputs.push_back(code);
        }
    }
    if (toggleInputs.empty()) toggleInputs = {16, 17, ui->monitors.detectedTypeCCode};
    draft.toggleInputs = toggleInputs;

    for (int i = 0; i < 4; ++i) {
        const std::string command = trim(gtk_entry_get_text(GTK_ENTRY(ui->cmdEntries[i])));
        if (command.empty()) draft.commands.erase(ui->cmdCodes[i]);
        else draft.commands[ui->cmdCodes[i]] = command;
    }

    for (auto& preset : draft.presets) preset.name = trim(preset.name);
    std::string error;
    if (!validatePresets(draft, &error)) {
        showMessage(GTK_WINDOW(ui->window), GTK_MESSAGE_WARNING, "KVMSwitch 设置", error);
        return;
    }
    if (!saveConfig(ui->app->cfgPath, draft, ui->monitors.detectedTypeCCode)) {
        showMessage(GTK_WINDOW(ui->window), GTK_MESSAGE_ERROR, "KVMSwitch 设置",
                    "保存配置文件失败:\n" + ui->app->cfgPath);
        return;
    }

    const bool autostart = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->autostartCheck)) != FALSE;
    if (!setAutostart(autostart, &error)) {
        showMessage(GTK_WINDOW(ui->window), GTK_MESSAGE_WARNING, "KVMSwitch 设置",
                    "配置已保存，但修改自启动失败:\n" + error);
    }

    GuiState* state = ui->app;
    state->cfg = loadConfig(state->cfgPath, state->ctx.detectedTypeCCode);
    refreshData(state, false);
    rebuildMenu(state);
    updateTrayTitle(state);
    if (state->cfg.notify) notifyDesktop("KVMSwitch 设置", "设置已保存并生效。");
    gtk_widget_destroy(ui->window);
}

void onSettingsCancel(GtkButton*, gpointer data) {
    auto* ui = static_cast<SettingsUi*>(data);
    gtk_widget_destroy(ui->window);
}

void showSettingsWindowInternal(GuiState* state, bool standalone) {
    if (!state) return;
    if (state->settingsWindow) {
        gtk_window_present(GTK_WINDOW(state->settingsWindow));
        return;
    }

    if (!standalone) refreshData(state);

    auto* ui = new SettingsUi();
    ui->app = state;
    ui->draft = state->cfg;
    ui->monitors = state->ctx;
    ui->standalone = standalone;

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    ui->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "KVMSwitch 设置");
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 560);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    const std::string icon = getIconPath();
    if (!icon.empty()) gtk_window_set_icon_from_file(GTK_WINDOW(window), icon.c_str(), nullptr);

    g_object_set_data_full(G_OBJECT(window), "settings-ui", ui, deleteSettingsUi);
    g_signal_connect(window, "destroy", G_CALLBACK(onSettingsDestroy), nullptr);
    state->settingsWindow = window;

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(outer), notebook, TRUE, TRUE, 0);
    buildPresetTab(ui, notebook);
    buildGeneralTab(ui, notebook);

    GtkWidget* bottom = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(bottom), GTK_BUTTONBOX_END);
    gtk_container_set_border_width(GTK_CONTAINER(bottom), 10);
    GtkWidget* cancelButton = gtk_button_new_with_label("取消");
    GtkWidget* saveButton = gtk_button_new_with_label("保存");
    gtk_widget_set_can_default(saveButton, TRUE);
    gtk_box_pack_end(GTK_BOX(bottom), saveButton, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), cancelButton, FALSE, FALSE, 0);
    g_signal_connect(saveButton, "clicked", G_CALLBACK(onSettingsSave), ui);
    g_signal_connect(cancelButton, "clicked", G_CALLBACK(onSettingsCancel), ui);
    gtk_box_pack_start(GTK_BOX(outer), bottom, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(window), outer);
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
}

void showSettingsWindow(GuiState* state) {
    showSettingsWindowInternal(state, false);
}


// ------------------------------------------------------------
// 单次快速切换窗口
// ------------------------------------------------------------

struct QuickAction {
    GuiState* state = nullptr;
    GtkWidget* window = nullptr;
    size_t monitorIndex = 0;
    uint16_t code = 0;
    bool toggle = false;
    bool preset = false;
    std::string presetName;
};

void deleteQuickAction(gpointer data) { delete static_cast<QuickAction*>(data); }

struct QuickRebuildData {
    GuiState* state = nullptr;
    GtkWidget* window = nullptr;
};

gboolean rebuildQuickIdle(gpointer data);

void finishQuickAction(QuickAction* action) {
    const bool standalone = GPOINTER_TO_INT(
                                g_object_get_data(G_OBJECT(action->window), "kvm-standalone")) != 0;
    gtk_widget_destroy(action->window);
    if (standalone) gtk_main_quit();
}

void onQuickSwitch(GtkButton* button, gpointer) {
    auto* action = static_cast<QuickAction*>(g_object_get_data(G_OBJECT(button), "kvm-quick-action"));
    if (!action) return;
    auto& monitor = action->state->ctx.monitors[action->monitorIndex];
    std::string error;
    if (switchMonitor(monitor, action->code, &error)) {
        monitor.currentInput = action->code;
        if (auto it = action->state->cfg.commands.find(action->code);
            it != action->state->cfg.commands.end()) {
            executeShellCommand(it->second);
        }
        if (action->state->cfg.notify) {
            notifyDesktop("KVMSwitch 输入切换",
                          monitor.description + " 已切换至: " +
                              getInputName(action->code, &action->state->cfg));
        }
    } else {
        showMessage(GTK_WINDOW(action->window), GTK_MESSAGE_ERROR, "KVMSwitch 错误",
                    "切换显示器输入源失败：\n" + error);
        return;
    }
    finishQuickAction(action);
}

void onQuickToggle(GtkButton* button, gpointer) {
    auto* action = static_cast<QuickAction*>(g_object_get_data(G_OBJECT(button), "kvm-quick-action"));
    if (!action) return;
    std::string error;
    if (!toggleInput(action->state->cfg, action->state->ctx,
                     action->state->ctx.detectedTypeCCode, &error)) {
        showMessage(GTK_WINDOW(action->window), GTK_MESSAGE_ERROR, "KVMSwitch 错误",
                    "轮换切换失败：\n" + error);
        return;
    }
    finishQuickAction(action);
}

void onQuickPreset(GtkButton* button, gpointer) {
    auto* action = static_cast<QuickAction*>(g_object_get_data(G_OBJECT(button), "kvm-quick-action"));
    if (!action) return;
    for (const auto& preset : action->state->cfg.presets) {
        if (preset.name == action->presetName) {
            std::string summary;
            applyPreset(action->state->cfg, action->state->ctx, preset,
                        action->state->ctx.detectedTypeCCode, &summary);
            break;
        }
    }
    finishQuickAction(action);
}

void quickAddButton(GtkWidget* box, const std::string& label, GtkWidget* window,
                    GuiState* state, QuickAction* action) {
    GtkWidget* button = gtk_button_new_with_label(label.c_str());
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    g_object_set_data_full(G_OBJECT(button), "kvm-quick-action", action, deleteQuickAction);
    if (action->toggle) g_signal_connect(button, "clicked", G_CALLBACK(onQuickToggle), state);
    else if (action->preset) g_signal_connect(button, "clicked", G_CALLBACK(onQuickPreset), state);
    else g_signal_connect(button, "clicked", G_CALLBACK(onQuickSwitch), state);
    (void)window;
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
}

void onQuickRefreshClicked(GtkButton*, gpointer data) {
    auto* state = static_cast<GuiState*>(data);
    refreshData(state);
    if (state->quickWindow) {
        auto* rebuild = new QuickRebuildData{state, state->quickWindow};
        g_idle_add(rebuildQuickIdle, rebuild);
    }
}

void buildQuickContent(GuiState* state, GtkWidget* window, GtkWidget* content, bool standalone) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(content));
    for (GList* node = children; node; node = node->next) gtk_widget_destroy(GTK_WIDGET(node->data));
    g_list_free(children);

    GtkWidget* header = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(header), "<b>KVMSwitch 快速切换</b>");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), header, FALSE, FALSE, 0);

    if (!state->cfg.presets.empty()) {
        GtkWidget* presetLabel = gtk_label_new("预设方案");
        gtk_widget_set_halign(presetLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(content), presetLabel, FALSE, FALSE, 4);
        for (const auto& preset : state->cfg.presets) {
            auto* action = new QuickAction{state, window, 0, 0, false, true, preset.name};
            quickAddButton(content, "▶ " + preset.name, window, state, action);
        }
        gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    }

    {
        auto* action = new QuickAction{state, window, 0, 0, true, false, {}};
        quickAddButton(content, "🔄 轮流切换输入源 (Toggle)", window, state, action);
    }

    if (state->ctx.monitors.empty()) {
        GtkWidget* empty = gtk_label_new("未检测到外接显示器");
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(content), empty, FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
        for (size_t i = 0; i < state->ctx.monitors.size(); ++i) {
            const auto& monitor = state->ctx.monitors[i];
            std::string label = "#" + std::to_string(monitor.index1) + " " + monitor.description;
            if (monitor.isPrimary) label += " [主]";
            label += "  [当前: " + monitorCurrentName(monitor, state->cfg) + "]";
            GtkWidget* monitorLabel = gtk_label_new(label.c_str());
            gtk_widget_set_halign(monitorLabel, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(content), monitorLabel, FALSE, FALSE, 4);

            GtkWidget* grid = gtk_grid_new();
            gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
            gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
            int col = 0;
            for (uint16_t code : getSupportedInputCodes(monitor, state->ctx.detectedTypeCCode)) {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "0x%02X", code);
                auto* action = new QuickAction{state, window, i, code, false, false, {}};
                GtkWidget* button = gtk_button_new_with_label(
                    (getInputName(code, &state->cfg) + "  (" + hex + ")").c_str());
                g_object_set_data_full(G_OBJECT(button), "kvm-quick-action", action, deleteQuickAction);
                g_signal_connect(button, "clicked", G_CALLBACK(onQuickSwitch), state);
                gtk_grid_attach(GTK_GRID(grid), button, col % 2, col / 2, 1, 1);
                ++col;
            }
            gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);
        }
    }

    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    GtkWidget* bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* refresh = gtk_button_new_with_label("刷新");
    GtkWidget* settings = gtk_button_new_with_label("设置...");
    GtkWidget* close = gtk_button_new_with_label("关闭");
    gtk_box_pack_end(GTK_BOX(bottom), close, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), settings, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), bottom, FALSE, FALSE, 0);

    g_signal_connect(refresh, "clicked", G_CALLBACK(onQuickRefreshClicked), state);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), window);
    if (standalone) {
        g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_main_quit), nullptr);
        g_signal_connect(settings, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* state = static_cast<GuiState*>(data);
            showSettingsWindowInternal(state, false);
        }), state);
    } else {
        g_signal_connect(settings, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* state = static_cast<GuiState*>(data);
            showSettingsWindowInternal(state, false);
        }), state);
    }
    gtk_widget_show_all(content);
}

GtkWidget* createQuickWindow(GuiState* state, bool standalone) {
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "KVMSwitch 快速切换");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 420);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    const std::string icon = getIconPath();
    if (!icon.empty()) gtk_window_set_icon_from_file(GTK_WINDOW(window), icon.c_str(), nullptr);

    g_object_set_data(G_OBJECT(window), "kvm-standalone", GINT_TO_POINTER(standalone ? 1 : 0));
    state->quickWindow = window;
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);
    g_object_set_data(G_OBJECT(window), "kvm-quick-content", content);
    gtk_container_add(GTK_CONTAINER(scroller), content);
    gtk_container_add(GTK_CONTAINER(window), scroller);
    buildQuickContent(state, window, content, standalone);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget* widget, gpointer data) {
        auto* s = static_cast<GuiState*>(data);
        if (s->quickWindow == widget) s->quickWindow = nullptr;
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "kvm-standalone")) != 0) {
            gtk_main_quit();
        }
    }), state);
    return window;
}

gboolean rebuildQuickIdle(gpointer data) {
    auto* rebuild = static_cast<QuickRebuildData*>(data);
    GuiState* state = rebuild->state;
    GtkWidget* window = rebuild->window;
    delete rebuild;

    if (!state->quickWindow || state->quickWindow != window) return G_SOURCE_REMOVE;
    GtkWidget* content = static_cast<GtkWidget*>(
        g_object_get_data(G_OBJECT(window), "kvm-quick-content"));
    if (!content) return G_SOURCE_REMOVE;
    const bool standalone = GPOINTER_TO_INT(
                                g_object_get_data(G_OBJECT(window), "kvm-standalone")) != 0;
    buildQuickContent(state, window, content, standalone);
    return G_SOURCE_REMOVE;
}

} // namespace

int runQuickMenuApp(const std::string& cfgPath, AppConfig cfg, MonitorContext ctx) {
    auto state = std::make_unique<GuiState>();
    state->cfgPath = cfgPath;
    state->cfg = std::move(cfg);
    state->ctx = std::move(ctx);

    if (!gtk_init_check(nullptr, nullptr)) {
        std::fprintf(stderr, "无法初始化 GTK：请确认已安装 GTK3 并在图形桌面会话中运行。\n");
        return 1;
    }

    refreshData(state.get(), true);

    GtkWidget* window = createQuickWindow(state.get(), true);
    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}

int runSettingsApp(const std::string& cfgPath, AppConfig cfg, MonitorContext ctx) {
    auto state = std::make_unique<GuiState>();
    state->cfgPath = cfgPath;
    state->cfg = std::move(cfg);
    state->ctx = std::move(ctx);

    if (!gtk_init_check(nullptr, nullptr)) {
        std::fprintf(stderr, "无法初始化 GTK：请确认已安装 GTK3 并在图形桌面会话中运行。\n");
        return 1;
    }

    refreshData(state.get(), true);
    showSettingsWindowInternal(state.get(), true);
    gtk_main();
    return 0;
}

} // namespace kvm

#else // !HAVE_GTK

#include <cstdio>

namespace kvm {

int runTrayApp(const std::string&, AppConfig, MonitorContext) {
    std::fprintf(stderr, "KVMSwitch 编译时未找到 GTK3，托盘模式不可用。\n"
                         "请安装 gtk3 与 libayatana-appindicator (开发包) 后重新执行 linux/build.sh。\n");
    return 1;
}

int runQuickMenuApp(const std::string&, AppConfig, MonitorContext) {
    std::fprintf(stderr, "KVMSwitch 编译时未找到 GTK3，快速切换窗口不可用。\n");
    return 1;
}

int runSettingsApp(const std::string&, AppConfig, MonitorContext) {
    std::fprintf(stderr, "KVMSwitch 编译时未找到 GTK3，图形设置窗口不可用。\n");
    return 1;
}

} // namespace kvm

#endif // HAVE_GTK
