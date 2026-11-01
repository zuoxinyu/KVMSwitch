// ddc.cpp : Linux i2c-dev DDC/CI 底层通信
#include "kvmswitch.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace kvm {

namespace {

constexpr uint8_t kHostAddress = 0x51;
constexpr uint8_t kI2cSlaveAddress = 0x37; // 0x6E >> 1
constexpr uint8_t kRequestSeed = 0x6E;     // 目标地址字节
constexpr uint8_t kReplySeed = 0x50;       // DDC/CI 回复校验种子

uint8_t checksum(uint8_t seed, const uint8_t* data, size_t len) {
    uint8_t value = seed;
    for (size_t i = 0; i < len; ++i) value ^= data[i];
    return value;
}

std::string errnoString(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

} // namespace

DdcBus::DdcBus(std::string devicePath) : path_(std::move(devicePath)) {}

DdcBus::~DdcBus() {
    close();
}

bool DdcBus::open(std::string* error) {
    close();
    fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        if (error) *error = errnoString("无法打开 " + path_);
        return false;
    }

    // 防止托盘进程与命令行进程同时访问同一条 DDC 总线
    if (flock(fd_, LOCK_EX) != 0) {
        if (error) *error = errnoString("无法锁定 " + path_);
        close();
        return false;
    }

    if (ioctl(fd_, I2C_SLAVE, kI2cSlaveAddress) < 0) {
        if (error) *error = errnoString("无法设置 DDC 从机地址 (检查 i2c 权限)");
        close();
        return false;
    }

    // ddcutil 也会先发送一个 0 字节唤醒/探测 DDC 通道。
    // 部分显卡的 AUX 通道在首次访问时需要这次事务，失败不致命。
    const uint8_t wake = 0x00;
    (void)!::write(fd_, &wake, 1);
    ::usleep(30000);
    return true;
}

void DdcBus::close() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
}

bool DdcBus::writeRequest(const std::vector<uint8_t>& req, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DDC 总线未打开";
        return false;
    }
    size_t written = 0;
    while (written < req.size()) {
        const ssize_t n = ::write(fd_, req.data() + written, req.size() - written);
        if (n <= 0) {
            if (error) *error = errnoString("DDC 写入失败");
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

bool DdcBus::readReply(uint8_t expectedOpcode, uint8_t expectedVcp,
                       std::vector<uint8_t>* reply, int* replyOffset,
                       uint8_t offsetHi, uint8_t offsetLo,
                       std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DDC 总线未打开";
        return false;
    }

    std::vector<uint8_t> buffer(256);
    std::vector<uint8_t> best;
    std::vector<uint8_t> loose;
    int bestOffset = -1;

    // AMDGPU 等驱动的 AUX 通道会在一次 read 中返回一个循环缓冲区，
    // 真正的回复可能出现在任意偏移，因此需要扫描同步字节并校验。
    // 不同驱动的 i2c-dev 对一次读取长度容忍度不同，失败时逐步缩小。
    static const size_t kReadSizes[] = {256, 128, 64, 40, 32, 16, 11};

    for (int attempt = 0; attempt < 4; ++attempt) {
        ssize_t n = -1;
        for (size_t size : kReadSizes) {
            n = ::read(fd_, buffer.data(), size);
            if (n > 0) break;
            if (errno != EIO && errno != EAGAIN) break;
        }
        if (n <= 0) {
            if (error) *error = errnoString("DDC 读取失败");
            break;
        }
        for (int i = 0; i + 11 <= static_cast<int>(n); ++i) {
            if (buffer[i] != 0x6E && buffer[i] != 0x50) continue;

            const uint8_t opcode = buffer[i + 1];
            const bool isVcp = (opcode == 0x88);
            const bool isCaps = (opcode == 0x83 || opcode == 0xA3);
            if (expectedOpcode == 0x88 && !isVcp) continue;
            if (expectedOpcode == 0x83 && !isCaps) continue;

            if (isVcp) {
                if (i + 11 > n) continue;
                if (buffer[i + 4] != expectedVcp) continue;
                if (checksum(kReplySeed, buffer.data() + i, 10) == buffer[i + 10]) {
                    best.assign(buffer.begin() + i, buffer.begin() + i + 11);
                    bestOffset = i;
                } else if (loose.empty() && buffer[i + 10] != 0) {
                    loose.assign(buffer.begin() + i, buffer.begin() + i + 11);
                }
            } else {
                // Capabilities 回复: 6e a3 e3 offHi offLo + 32 数据 + 校验和
                if (i + 38 > n) continue;
                if (buffer[i + 3] != offsetHi || buffer[i + 4] != offsetLo) continue;
                if (checksum(kReplySeed, buffer.data() + i, 37) == buffer[i + 37]) {
                    best.assign(buffer.begin() + i, buffer.begin() + i + 38);
                    bestOffset = i;
                } else if (loose.empty() && buffer[i + 37] != 0) {
                    loose.assign(buffer.begin() + i, buffer.begin() + i + 38);
                }
            }
        }

        if (!best.empty()) {
            if (reply) *reply = std::move(best);
            if (replyOffset) *replyOffset = bestOffset;
            return true;
        }
        ::usleep(20000);
    }

    // 少数显示器校验和字段不规范：使用“存在非零校验和且头部匹配”的宽松结果。
    if (!loose.empty()) {
        if (reply) *reply = std::move(loose);
        if (replyOffset) *replyOffset = 0;
        return true;
    }

    if (error && error->empty()) *error = "显示器未返回有效的 DDC/CI 回复";
    return false;
}

std::optional<uint16_t> DdcBus::getVcp(uint8_t code, std::string* error) {
    std::vector<uint8_t> req = {kHostAddress, 0x82, 0x01, code, 0x00};
    req[4] = checksum(kRequestSeed, req.data(), req.size() - 1);

    std::string lastError;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string writeError;
        if (!writeRequest(req, &writeError)) {
            lastError = writeError;
            break;
        }
        ::usleep(50000 + attempt * 20000);

        std::vector<uint8_t> reply;
        int offset = 0;
        std::string readError;
        if (readReply(0x88, code, &reply, &offset, 0, 0, &readError)) {
            return static_cast<uint16_t>((reply[8] << 8) | reply[9]);
        }
        lastError = readError;
        ::usleep(30000);
    }

    if (error) *error = lastError.empty() ? "读取 VCP 0x60 失败" : lastError;
    return std::nullopt;
}

bool DdcBus::setVcp(uint8_t code, uint16_t value, std::string* error) {
    // SetVCP: 51 84 03 <code> <hi> <lo> <checksum>
    std::vector<uint8_t> req = {
        kHostAddress, 0x84, 0x03, code,
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF), 0x00};
    req[6] = checksum(kRequestSeed, req.data(), req.size() - 1);

    std::string lastError;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string writeError;
        if (writeRequest(req, &writeError)) {
            // DDC SetVCP 不要求显示器回包；等待固件处理后再结束。
            ::usleep(60000);
            return true;
        }
        lastError = writeError;
        ::usleep(80000);
    }
    if (error) *error = lastError.empty() ? "写入 VCP 0x60 失败" : lastError;
    return false;
}

std::string DdcBus::getCapabilities(std::string* error) {
    std::string capabilities;
    std::string lastError;

    for (int offset = 0; offset < 0x400; offset += 0x20) {
        std::vector<uint8_t> req = {
            kHostAddress, 0x83, 0xF3,
            static_cast<uint8_t>((offset >> 8) & 0xFF),
            static_cast<uint8_t>(offset & 0xFF), 0x00};
        req[5] = checksum(kRequestSeed, req.data(), req.size() - 1);

        bool received = false;
        std::vector<uint8_t> reply;
        for (int attempt = 0; attempt < 3 && !received; ++attempt) {
            std::string writeError;
            if (!writeRequest(req, &writeError)) {
                lastError = writeError;
                break;
            }
            ::usleep(60000);

            int replyOffset = 0;
            std::string readError;
            if (readReply(0x83, 0, &reply, &replyOffset,
                          static_cast<uint8_t>((offset >> 8) & 0xFF),
                          static_cast<uint8_t>(offset & 0xFF), &readError)) {
                received = true;
            } else {
                lastError = readError;
                ::usleep(30000);
            }
        }

        if (!received) {
            if (capabilities.empty() && error) *error = lastError;
            break;
        }

        // 回复固定带 32 字节数据（末段可能用 0 填充）
        bool terminated = false;
        for (size_t i = 5; i < reply.size() - 1; ++i) {
            const uint8_t ch = reply[i];
            if (ch == 0x00) {
                terminated = true;
                break;
            }
            capabilities.push_back(static_cast<char>(ch));
        }
        if (terminated) break;
    }

    if (capabilities.empty() && error && error->empty()) *error = lastError;
    return capabilities;
}

// ------------------------------------------------------------
// EDID 辅助
// ------------------------------------------------------------

std::string readEdidName(const std::string& edidPath) {
    FILE* file = std::fopen(edidPath.c_str(), "rb");
    if (!file) return {};
    uint8_t edid[512] = {0};
    const size_t n = std::fread(edid, 1, sizeof(edid), file);
    std::fclose(file);
    if (n < 128) return {};

    // EDID 描述符 0xFC: 显示器名称
    for (size_t offset = 54; offset + 18 <= 126; offset += 18) {
        if (edid[offset] == 0x00 && edid[offset + 1] == 0x00 &&
            edid[offset + 2] == 0x00 && edid[offset + 3] == 0xFC) {
            std::string name;
            for (size_t i = 5; i < 18; ++i) {
                const uint8_t ch = edid[offset + i];
                if (ch == 0x0A || ch == 0x00) break;
                if (ch >= 0x20 && ch <= 0x7E) name.push_back(static_cast<char>(ch));
            }
            name = trim(name);
            if (!name.empty()) return name;
        }
    }

    // 回退: 厂商 ID + 产品代码
    if (n >= 12) {
        char mfg[4] = {0};
        mfg[0] = static_cast<char>(((edid[8] >> 2) & 0x1F) + 'A' - 1);
        mfg[1] = static_cast<char>((((edid[8] & 0x03) << 3) | (edid[9] >> 5)) + 'A' - 1);
        mfg[2] = static_cast<char>((edid[9] & 0x1F) + 'A' - 1);
        const uint16_t product = static_cast<uint16_t>(edid[10] | (edid[11] << 8));
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s %04X", mfg, product);
        return buffer;
    }
    return {};
}

std::string hashEdid(const std::string& edidPath) {
    FILE* file = std::fopen(edidPath.c_str(), "rb");
    if (!file) return {};
    uint8_t edid[256] = {0};
    const size_t n = std::fread(edid, 1, sizeof(edid), file);
    std::fclose(file);
    if (n < 128) return {};

    uint64_t hash = 1469598103934665603ULL; // FNV-1a
    for (size_t i = 0; i < n; ++i) {
        hash ^= edid[i];
        hash *= 1099511628211ULL;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

} // namespace kvm
