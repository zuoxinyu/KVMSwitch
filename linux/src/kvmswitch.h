// kvmswitch.h : Linux 端公共类型与接口声明
// 基于 Linux i2c-dev 直接访问 DDC/CI，UI 使用 GTK3 + Ayatana AppIndicator（可选）
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kvm {

// ------------------------------------------------------------
// 数据结构
// ------------------------------------------------------------

// 多显示器预设目标项（对应单个显示器的目标输入源）
struct PresetTarget {
    std::string monitorId;   // "1", "2", "primary", "secondary", 设备名或显示器描述
    uint16_t inputCode = 0;  // 目标 VCP 0x60 代码（0 表示保持不变）
};

// 多显示器预设方案
struct MonitorPreset {
    std::string name;
    std::vector<PresetTarget> targets;
};

struct AppConfig {
    std::string mode = "tray";          // "tray" / "menu" / "toggle"
    bool notify = true;                 // 是否显示桌面通知
    std::string targetMonitor = "primary"; // "primary" / "all"
    std::vector<uint16_t> toggleInputs; // toggle 轮换序列
    std::map<std::string, uint16_t> customInputs; // 输入源别名 -> VCP 代码
    std::map<uint16_t, std::string> commands;     // VCP 代码 -> 联动命令
    std::vector<MonitorPreset> presets;
};

// 一台通过 DDC/CI 可控制的显示器
struct MonitorInfo {
    std::string connector;    // 例如 "DP-2"
    std::string drmPath;      // 例如 "card1-DP-2"
    std::string i2cBus;       // 例如 "/dev/i2c-7"
    std::string edidName;     // EDID 0xFC 描述符中的型号名，例如 "P275MV"
    std::string description;  // 菜单展示名称
    std::string edidHash;     // EDID 的稳定哈希（能力缓存键）
    uint16_t currentInput = 0;
    std::vector<uint16_t> supportedInputs;
    bool isPrimary = false;
    bool isExternal = true;
    int index1 = 0;           // 1-based 外接显示器序号
};

struct MonitorContext {
    std::vector<MonitorInfo> monitors;
    uint16_t detectedTypeCCode = 15;
};

// ------------------------------------------------------------
// 字符串 / 文件 / 配置
// ------------------------------------------------------------

std::string trim(const std::string& s);
std::string toUpper(const std::string& s);
std::string toLower(const std::string& s);
std::vector<std::string> split(const std::string& s, char delimiter);
bool startsWith(const std::string& s, const std::string& prefix);
bool parseBool(const std::string& s, bool fallback);

std::string getExePath();
std::string getExeDir();
std::string getConfigPath();
std::string getRuntimeDir();
std::string getCacheDir();
std::string getIconPath();

void ensureConfigFile(const std::string& cfgPath);
AppConfig loadConfig(const std::string& cfgPath, uint16_t detectedTypeCCode = 15);
bool saveConfig(const std::string& cfgPath, const AppConfig& cfg, uint16_t detectedTypeCCode = 15);

uint16_t parseInputAlias(const std::string& text, const AppConfig& cfg, uint16_t detectedTypeCCode = 15);
std::string getInputName(uint16_t code, const AppConfig* cfg = nullptr);
std::string getInputAliasForCode(uint16_t code, uint16_t detectedTypeCCode = 15);
std::string getInputHex(uint16_t code);
std::string formatHexCode(uint16_t code);

// ------------------------------------------------------------
// DDC/CI (i2c-dev)
// ------------------------------------------------------------

class DdcBus {
public:
    explicit DdcBus(std::string devicePath);
    ~DdcBus();

    DdcBus(const DdcBus&) = delete;
    DdcBus& operator=(const DdcBus&) = delete;

    bool open(std::string* error = nullptr);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    // 读取 VCP 代码。失败返回 nullopt。
    std::optional<uint16_t> getVcp(uint8_t code, std::string* error = nullptr);
    // 写入 VCP 代码。DDC/CI SetVCP 通常不回包，write 成功即视为成功。
    bool setVcp(uint8_t code, uint16_t value, std::string* error = nullptr);
    // 读取显示器 Capabilities 字符串（可能耗时约 0.5-1 秒）。
    std::string getCapabilities(std::string* error = nullptr);

private:
    bool writeRequest(const std::vector<uint8_t>& req, std::string* error);
    bool readReply(uint8_t expectedOpcode, uint8_t expectedVcp,
                   std::vector<uint8_t>* reply, int* replyOffset,
                   uint8_t offsetHi = 0, uint8_t offsetLo = 0,
                   std::string* error = nullptr);

    std::string path_;
    int fd_ = -1;
};

// 从 EDID 中解析显示器名称
std::string readEdidName(const std::string& edidPath);
std::string hashEdid(const std::string& edidPath);

// ------------------------------------------------------------
// 显示器枚举与操作
// ------------------------------------------------------------

// 枚举所有支持 DDC/CI 的外接显示器并读取当前状态。
// readCapabilities 为 false 时跳过耗时的 Capabilities 查询（使用缓存/默认值）。
MonitorContext queryMonitors(const AppConfig& cfg, bool readCapabilities = true,
                             uint16_t detectedTypeCCode = 15, bool useCapsCache = true);

// 不读取硬件状态，仅枚举连接器与 DDC 总线（用于设置窗口快速展示）。
std::vector<MonitorInfo> enumerateMonitors();

// 根据当前显示器重新探测 Type-C VCP 代码。
uint16_t detectTypeCCode(const MonitorContext& ctx);

// 强制刷新所有显示器的当前输入源。
void refreshMonitorStates(MonitorContext& ctx);

// 判断预设目标是否匹配某台显示器
bool matchMonitor(const MonitorInfo& mon, const std::string& targetId);

std::vector<uint16_t> getSupportedInputCodes(const MonitorInfo& mon, uint16_t detectedTypeCCode);
std::vector<std::string> getSupportedInputNames(const MonitorInfo& mon, const AppConfig& cfg,
                                                uint16_t detectedTypeCCode);

// 切换单台显示器输入源（带重试）
bool switchMonitor(const MonitorInfo& mon, uint16_t code, std::string* error = nullptr);

// 切换目标显示器（primary / all）
int switchTargetMonitors(const AppConfig& cfg, MonitorContext& ctx, uint16_t code,
                         std::string* error = nullptr);

// 应用多显示器预设
int applyPreset(const AppConfig& cfg, MonitorContext& ctx, const MonitorPreset& preset,
                uint16_t detectedTypeCCode, std::string* summary = nullptr);

// 一键轮换（基于硬件实时状态）
bool toggleInput(const AppConfig& cfg, MonitorContext& ctx, uint16_t detectedTypeCCode,
                 std::string* error = nullptr);

// 保存当前各屏状态为新预设
bool saveCurrentStateAsPreset(const std::string& cfgPath, MonitorContext& ctx,
                              const std::string& presetName, uint16_t detectedTypeCCode,
                              std::string* error = nullptr);

// 将预设目标列表格式化为可读摘要
std::string presetSummary(const MonitorPreset& preset, const AppConfig& cfg);

// ------------------------------------------------------------
// 系统集成（通知 / 命令 / 自启动 / 快捷方式 / IPC）
// ------------------------------------------------------------

void logMsg(const std::string& msg);
void notifyDesktop(const std::string& title, const std::string& message);
void executeShellCommand(const std::string& command);

bool isAutostartEnabled();
bool setAutostart(bool enable, std::string* error = nullptr);
bool createDesktopShortcuts(const AppConfig& cfg, std::string* message = nullptr);

bool sendTrayCommand(const std::string& command);
std::string traySocketPath();
bool trayIsRunning();

void openPathWithDesktop(const std::string& path);

// 通过 xrandr / kscreen-doctor 探测主显示器连接器名称（失败返回空）
std::string detectPrimaryConnector();

// ------------------------------------------------------------
// GUI（由 gui.cpp 实现；未编译 GTK 时返回 false）
// ------------------------------------------------------------

// 常驻托盘模式
int runTrayApp(const std::string& cfgPath, AppConfig cfg, MonitorContext ctx);
// 单次快速切换窗口
int runQuickMenuApp(const std::string& cfgPath, AppConfig cfg, MonitorContext ctx);
// 独立设置窗口
int runSettingsApp(const std::string& cfgPath, AppConfig cfg, MonitorContext ctx);

} // namespace kvm
