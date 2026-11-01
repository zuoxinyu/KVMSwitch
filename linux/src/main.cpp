// main.cpp : 命令行入口与单次执行逻辑
#include "kvmswitch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <unistd.h>

namespace {

const char* kVersion = "1.0.0";

void printHelp() {
    std::cout <<
        "KVMSwitch " << kVersion << " - Linux 显示器输入源快速切换工具 (DDC/CI)\n\n"
        "用法:\n"
        "  KVMSwitch                       根据 config.ini 的 mode 运行 (默认常驻托盘)\n"
        "  KVMSwitch --tray                强制以系统托盘常驻模式启动\n"
        "  KVMSwitch dp                    直接切换到 DisplayPort (DP)\n"
        "  KVMSwitch hdmi1                 直接切换到 HDMI 1\n"
        "  KVMSwitch hdmi2                 直接切换到 HDMI 2\n"
        "  KVMSwitch typec                 直接切换到 USB Type-C\n"
        "  KVMSwitch <数值>                直接切换到指定 VCP 0x60 数值 (如 15, 16, 17)\n"
        "  KVMSwitch --preset <名称> (-p)  一键应用多显示器预设方案\n"
        "  KVMSwitch --presets             列出所有已配置的预设方案\n"
        "  KVMSwitch --toggle (-t)         在 toggle_inputs 中轮换切换\n"
        "  KVMSwitch --menu (-m)           弹出单次快速切换窗口\n"
        "  KVMSwitch --settings (-s)       打开图形化设置窗口\n"
        "  KVMSwitch --query (-q)          探测并显示当前显示器与输入源状态\n"
        "  KVMSwitch --autostart-enable    开启开机自启动 (XDG Autostart)\n"
        "  KVMSwitch --autostart-disable   关闭开机自启动\n"
        "  KVMSwitch --autostart-status    查询开机自启动状态\n"
        "  KVMSwitch --create-shortcuts    在桌面生成一键切换快捷方式\n"
        "  KVMSwitch --open-config         打开 config.ini\n"
        "  KVMSwitch --quit                退出正在运行的托盘实例\n"
        "  KVMSwitch --help (-h)           查看帮助信息\n\n"
        "配置:\n"
        "  默认使用程序旁的 config.ini；若程序目录只读，则使用\n"
        "  $XDG_CONFIG_HOME/kvmswitch/config.ini。可用 KVMSWITCH_CONFIG 环境变量覆盖。\n\n"
        "权限:\n"
        "  需要当前用户对 /dev/i2c-* 有读写权限。若提示权限不足，请安装\n"
        "  linux/60-kvmswitch-i2c.rules 到 /etc/udev/rules.d/ 并重新加载 udev，\n"
        "  或把用户加入 i2c 组 (需要重新登录)。\n";
}

void printQuery(const kvm::MonitorContext& ctx, const kvm::AppConfig& cfg) {
    std::cout << "KVMSwitch 显示器检测\n";
    std::cout << "发现 " << ctx.monitors.size() << " 台支持 DDC/CI 的外接显示器\n";
    if (ctx.monitors.empty()) {
        std::cout << "\n未检测到显示器。请检查:\n"
                     "  1. 显示器 OSD 菜单中已开启 DDC/CI\n"
                     "  2. 显示器通过 DP / HDMI / Type-C 连接且桌面已点亮\n"
                     "  3. 当前用户对 /dev/i2c-* 有读写权限\n";
        return;
    }

    for (const auto& monitor : ctx.monitors) {
        std::cout << "\n[" << monitor.index1 << "] " << monitor.description;
        if (monitor.isPrimary) std::cout << " (主显示器)";
        std::cout << "\n";
        std::cout << "    连接器: " << monitor.connector << "  (" << monitor.drmPath << ")\n";
        std::cout << "    DDC 总线: " << monitor.i2cBus << "\n";
        std::cout << "    当前输入: "
                  << (monitor.currentInput ? kvm::getInputName(monitor.currentInput, &cfg) : "未知")
                  << " (" << kvm::formatHexCode(monitor.currentInput) << ")\n";
        std::cout << "    支持输入: ";
        const auto codes = kvm::getSupportedInputCodes(monitor, ctx.detectedTypeCCode);
        for (size_t i = 0; i < codes.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << kvm::getInputName(codes[i], &cfg) << " (" << kvm::formatHexCode(codes[i]) << ")";
        }
        std::cout << "\n";
    }
    std::cout << "\nType-C VCP 代码: " << kvm::formatHexCode(ctx.detectedTypeCCode)
              << " (检测自显示器 Capabilities)\n";
}

void printPresets(const kvm::AppConfig& cfg) {
    if (cfg.presets.empty()) {
        std::cout << "未配置任何预设方案。您可以在 config.ini 的 [Presets] 节添加，"
                     "或在托盘菜单中保存当前状态为预设。\n";
        return;
    }
    std::cout << "当前共配置了 " << cfg.presets.size() << " 个多显示器预设方案：\n\n";
    for (size_t i = 0; i < cfg.presets.size(); ++i) {
        std::cout << "[" << (i + 1) << "] " << kvm::presetSummary(cfg.presets[i], cfg) << "\n";
    }
}

const kvm::MonitorPreset* findPreset(const kvm::AppConfig& cfg, const std::string& name) {
    const std::string target = kvm::toUpper(kvm::trim(name));
    if (target.empty()) return nullptr;
    for (const auto& preset : cfg.presets) {
        if (kvm::toUpper(preset.name) == target) return &preset;
    }
    for (const auto& preset : cfg.presets) {
        if (kvm::toUpper(preset.name).find(target) != std::string::npos) return &preset;
    }
    return nullptr;
}

std::string joinArgs(const std::vector<std::string>& args, size_t start) {
    std::string result;
    for (size_t i = start; i < args.size(); ++i) {
        if (!result.empty()) result += " ";
        result += args[i];
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string rawFirst = args.empty() ? std::string() : args[0];
    const std::string command = kvm::toLower(kvm::trim(rawFirst));

    const std::string cfgPath = kvm::getConfigPath();

    // --help / 版本无需读取硬件
    if (command == "--help" || command == "-h" || command == "/?" || command == "help") {
        printHelp();
        return 0;
    }
    if (command == "--version" || command == "-v" || command == "version") {
        std::cout << "KVMSwitch " << kVersion << " (Linux)\n";
        return 0;
    }

    // 解析 --preset <名称> / -p <名称> / --preset=<名称>
    bool presetRequest = false;
    std::string presetName;
    if (command == "--preset" || command == "-p") {
        presetRequest = true;
        presetName = joinArgs(args, 1);
    } else if (kvm::startsWith(command, "--preset=")) {
        presetRequest = true;
        presetName = kvm::trim(rawFirst.substr(std::string("--preset=").size()));
    }

    // 快速路径：托盘已在运行时，仅转发设置/菜单/退出请求，不重复枚举硬件
    const bool settingsRequest = (command == "--settings" || command == "-s" || command == "settings");
    const bool menuRequest = (command == "--menu" || command == "-m" || command == "menu");
    const bool quitRequest = (command == "--quit" || command == "quit");
    const bool trayRequest = (command == "--tray" || command == "-tray" || command == "tray");

    kvm::AppConfig initialCfg = kvm::loadConfig(cfgPath, 15);

    if (kvm::trayIsRunning()) {
        if (settingsRequest) {
            if (kvm::sendTrayCommand("show-settings")) return 0;
        } else if (menuRequest || trayRequest || (args.empty() && initialCfg.mode == "tray")) {
            if (kvm::sendTrayCommand("show-menu")) {
                std::cout << "KVMSwitch 已在运行，已请求托盘实例显示菜单。\n";
                return 0;
            }
        } else if (quitRequest) {
            if (kvm::sendTrayCommand("quit")) {
                std::cout << "已请求 KVMSwitch 托盘实例退出。\n";
                return 0;
            }
        }
    }

    if (quitRequest) {
        std::cout << "KVMSwitch 托盘实例未在运行。\n";
        return 0;
    }

    // 无需枚举硬件的命令
    if (command == "--autostart-enable" || command == "autostart-enable") {
        std::string error;
        if (!kvm::setAutostart(true, &error)) {
            std::cerr << "开启开机自启动失败: " << error << "\n";
            return 1;
        }
        std::cout << "开机自启动已启用 (登录桌面后自动常驻托盘)。\n";
        return 0;
    }
    if (command == "--autostart-disable" || command == "autostart-disable") {
        std::string error;
        if (!kvm::setAutostart(false, &error)) {
            std::cerr << "关闭开机自启动失败: " << error << "\n";
            return 1;
        }
        std::cout << "开机自启动已禁用。\n";
        return 0;
    }
    if (command == "--autostart-status" || command == "autostart-status") {
        std::cout << (kvm::isAutostartEnabled() ? "开机自启动状态: 已启用 (Enabled)\n"
                                                : "开机自启动状态: 已禁用 (Disabled)\n");
        return 0;
    }
    if (command == "--open-config" || command == "open-config") {
        kvm::ensureConfigFile(cfgPath);
        kvm::openPathWithDesktop(cfgPath);
        return 0;
    }
    if (command == "--create-shortcuts" || command == "create-shortcuts" || command == "shortcuts") {
        std::string message;
        if (!kvm::createDesktopShortcuts(initialCfg, &message)) {
            std::cerr << message << "\n";
            return 1;
        }
        std::cout << message << "\n";
        return 0;
    }
    if (command == "--presets" || command == "presets") {
        printPresets(initialCfg);
        return 0;
    }

    // 需要硬件枚举的命令：先以默认 Type-C 代码探测，再按 Capabilities 修正配置
    kvm::MonitorContext ctx = kvm::queryMonitors(initialCfg, true, 15, true);
    const uint16_t typeCCode = ctx.detectedTypeCCode;
    kvm::AppConfig cfg = kvm::loadConfig(cfgPath, typeCCode);
    ctx.detectedTypeCCode = typeCCode;

    if (command == "--query" || command == "-q" || command == "query") {
        printQuery(ctx, cfg);
        return 0;
    }

    if (settingsRequest) {
        return kvm::runSettingsApp(cfgPath, cfg, ctx);
    }
    if (command == "--tray" || command == "-tray" || command == "tray" ||
        (args.empty() && cfg.mode == "tray")) {
        return kvm::runTrayApp(cfgPath, cfg, ctx);
    }
    if (menuRequest || (args.empty() && cfg.mode == "menu")) {
        return kvm::runQuickMenuApp(cfgPath, cfg, ctx);
    }
    if (command == "--toggle" || command == "-t" || command == "toggle" ||
        (args.empty() && cfg.mode == "toggle")) {
        std::string error;
        if (!kvm::toggleInput(cfg, ctx, typeCCode, &error)) {
            std::cerr << "轮换切换失败: " << error << "\n";
            return 1;
        }
        kvm::sendTrayCommand("refresh");
        return 0;
    }

    // 预设或直接输入源
    bool directPreset = false;
    std::string directPresetName;
    if (presetRequest) {
        directPreset = true;
        directPresetName = presetName;
    } else if (!args.empty() && !kvm::startsWith(command, "-")) {
        // 允许直接运行 KVMSwitch 办公模式
        if (findPreset(cfg, rawFirst)) {
            directPreset = true;
            directPresetName = rawFirst;
        }
    }

    if (directPreset) {
        const kvm::MonitorPreset* preset = findPreset(cfg, directPresetName);
        if (!preset) {
            std::cerr << "未找到名为「" << directPresetName << "」的多显示器预设方案！\n"
                      << "请检查 config.ini 中的 [Presets] 配置，或运行 KVMSwitch --presets 查看列表。\n";
            return 1;
        }
        if (ctx.monitors.empty()) {
            std::cerr << "未检测到支持 DDC/CI 的显示器。\n";
            return 1;
        }
        std::string summary;
        kvm::applyPreset(cfg, ctx, *preset, typeCCode, &summary);
        std::cout << summary << "\n";
        kvm::sendTrayCommand("refresh");
        return 0;
    }

    std::string targetText = rawFirst;
    if (kvm::startsWith(targetText, "--")) targetText = targetText.substr(2);
    else if (kvm::startsWith(targetText, "-")) targetText = targetText.substr(1);

    const uint16_t targetCode = kvm::parseInputAlias(targetText, cfg, typeCCode);
    if (targetCode == 0) {
        std::cerr << "无法识别的输入源或预设参数: " << rawFirst << "\n\n"
                  << "支持的参数示例:\n"
                  << "• 输入源: dp, hdmi1, hdmi2, typec, 15, 16, 17\n"
                  << "• 预设方案: --preset 办公模式, --presets\n"
                  << "• 其他: --query, --toggle, --settings, --tray\n";
        return 1;
    }

    if (ctx.monitors.empty()) {
        std::cerr << "未检测到支持 DDC/CI 的显示器！\n\n"
                  << "请确认:\n"
                  << "1. 显示器 OSD 菜单中已开启 DDC/CI\n"
                  << "2. 显示器已通过 DP / HDMI / Type-C 连接\n"
                  << "3. 当前用户对 /dev/i2c-* 有读写权限 (参见 linux/60-kvmswitch-i2c.rules)\n";
        return 1;
    }

    std::string error;
    const int switched = kvm::switchTargetMonitors(cfg, ctx, targetCode, &error);
    if (switched == 0) {
        std::cerr << "切换输入源失败: " << error << "\n";
        return 1;
    }
    std::cout << "已切换至 " << kvm::getInputName(targetCode, &cfg)
              << " (" << kvm::formatHexCode(targetCode) << ")"
              << (switched > 1 ? "，所有显示器" : "") << "\n";
    kvm::sendTrayCommand("refresh");
    return 0;
}
