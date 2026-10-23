// ============================================================
// cli/AtomicWrite.h — CLI 文本文件原子写工具
// ------------------------------------------------------------
// F10-F13（2026-10-23）：fmt --write / lint --fix / doc -o / fuzz crash 导出
// 此前 std::ofstream 就地截断，崩溃/磁盘满会留下半截文件（fmt/lint 直接损坏
// 用户源码）。统一改为同目录临时文件 + rename 原子替换；rename 失败回退就地写。
// 保留目标文件权限位（POSIX 下替换 inode 不会丢 mode）。
// ============================================================
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace minilang_cli {

inline unsigned long long atomicWriteProcessId() {
#if defined(_WIN32)
    return static_cast<unsigned long long>(_getpid());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

inline std::filesystem::path pathFromUtf8(const std::string& path) {
#if defined(_WIN32)
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
#else
    return std::filesystem::path(path);
#endif
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.string();
#endif
}

/// 原子写文本文件：同目录临时文件 + rename 替换；rename 失败回退就地写。
/// 文本模式（保持既有换行语义）；失败时清理临时文件并返回 false。
inline bool atomicWriteFile(const std::string& path, const std::string& content, std::string* errorMessage = nullptr) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> sequence{0};
    fs::path dst = pathFromUtf8(path);
    // 符号链接：解析到真实目标后再原子替换，避免把链接本身替换成普通文件。
    // 硬链接不做特殊处理（原子替换 inode 会断开硬链接，属已知取舍）。
    std::error_code linkEc;
    if (fs::is_symlink(dst, linkEc) && !linkEc) {
        std::error_code canonEc;
        const fs::path resolved = fs::canonical(dst, canonEc);
        if (!canonEc)
            dst = resolved;
    }
    fs::path tmp = dst;
    tmp += ".tmp" + std::to_string(atomicWriteProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));

    auto fail = [&](const std::string& message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };

    {
        std::ofstream ofs(tmp, std::ios::out | std::ios::trunc);
        if (!ofs.is_open())
            return fail("无法创建临时文件: " + pathToUtf8(tmp));
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        if (!ofs.good()) {
            ofs.close();
            std::error_code cleanupEc;
            fs::remove(tmp, cleanupEc);
            return fail("写入临时文件失败: " + pathToUtf8(tmp));
        }
    }

    // 保留目标权限位（POSIX rename 替换 inode；Windows 权限模型不同但无副作用）
    std::error_code permEc;
    if (fs::exists(dst, permEc)) {
        const auto perms = fs::status(dst, permEc).permissions();
        if (!permEc)
            fs::permissions(tmp, perms, fs::perm_options::replace, permEc);
    }

    std::error_code renameEc;
    fs::rename(tmp, dst, renameEc);
    if (!renameEc)
        return true;

    // 回退：清理临时文件后就地写（旧行为，保留极端文件系统下的可用性）
    std::error_code cleanupEc;
    fs::remove(tmp, cleanupEc);
    std::ofstream ofs(dst, std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return fail("无法写入文件: " + path);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.flush();
    if (!ofs.good())
        return fail("写入文件失败: " + path);
    return true;
}

} // namespace minilang_cli
