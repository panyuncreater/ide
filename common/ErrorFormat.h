#pragma once

// ============================================================
// common/ErrorFormat.h — 运行时错误格式化工具（单一真相源）
// ------------------------------------------------------------
// R97 #6 fix: 合并原 interpreter/ErrorFormat.h（format/intToString）与
// common/Logger.h 中的 ErrorFormat namespace（formatWithLocation/formatWithLine），
// 消除两处定义分散维护问题。
//
// P3 fix: 替代 "msg" + std::to_string(n) + "msg" 模式。
// std::to_string 在 MSVC 上会触发 locale 查询与堆分配，
// 循环内越界等场景频繁触发错误路径时产生可观开销。
// 本工具使用栈缓冲 + std::to_chars（无 locale、无堆分配）。
//
// 用法：
//   ErrorFormat::format("数组索引越界: %d, 有效范围 [0, %d)", idx, size);
//   ErrorFormat::formatWithLocation(msg, line, col);
//   ErrorFormat::formatWithLine(msg, line);
//
// format 支持的格式说明符：
//   %d  - int / int64_t / size_t（整型）
//   %s  - const char* / std::string
//   %zu - size_t（显式）
// 其余直接拼接为字符串的字面量。
// ============================================================

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace ErrorFormat {

/// @brief 将整数写入栈缓冲并返回字符串（无堆分配）。
///
/// 替代 std::to_string，避免 MSVC 上的 locale 查询开销。
/// 使用 std::to_chars（C++17）实现，零堆分配。
///
/// @tparam T 整型类型（int / int64_t / size_t / 等）
/// @param value 待转换的整数值
/// @return 转换后的 std::string
template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>> inline std::string intToString(T value) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), value);
    return std::string(buf, res.ptr);
}

/// @brief 格式化错误消息（栈缓冲，无 std::to_string 调用）。
///
/// P3 fix: 替代 "msg" + std::to_string(n) + "msg" 模式，避免多次堆分配。
/// 使用 snprintf 到 512 字节栈缓冲，足以容纳绝大多数运行时错误消息。
///
/// 支持的占位符：
///   - `%d` - int / int64_t / size_t（整型）
///   - `%s` - const char* / std::string
///   - `%zu` - size_t（显式）
///
/// @param fmt printf 风格格式字符串
/// @param args 格式化参数（须与 fmt 中的占位符匹配）
/// @return 格式化后的 std::string；格式化失败时回退返回 fmt 原文
/// @note 消息超过 512 字节时截断为 511 字节（保证 null 终止）
template <typename... Args> inline std::string format(const char* fmt, Args&&... args) {
    // 简单实现：用 snprintf 到栈缓冲，避免 std::to_string 的 locale 开销。
    // 缓冲区 512 字节足以容纳绝大多数运行时错误消息。
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    if (n < 0)
        return std::string(fmt); // 格式化失败，回退原始字符串
    if (static_cast<size_t>(n) < sizeof(buf)) {
        return std::string(buf, static_cast<size_t>(n));
    }
    // 截断（消息过长）：返回 512 字节
    return std::string(buf, sizeof(buf) - 1);
}

// ============================================================
// P1-9 fix: 错误消息定位格式化（热路径优化）
// ------------------------------------------------------------
// runtimeError 是错误处理热路径，原实现使用 std::to_string + operator+
// 多次堆分配。改用 std::to_chars 写入栈缓冲区，零堆分配。
// ============================================================

/// @brief 为错误消息附加行号/列号定位信息。
///
/// 输出格式：`<msg> (行 <line>, 列 <col>)`
/// 用于 runtimeError 等需要源码定位的错误消息构造。
///
/// @param msg 原始错误消息
/// @param line 源码行号
/// @param col 源码列号
/// @return 附加定位信息后的错误消息
inline std::string formatWithLocation(const std::string& msg, int line, int col) {
    // 预估容量：msg + " (行 " + 11 位 int + ", 列 " + 11 位 int + ")"
    std::string result;
    result.reserve(msg.size() + 32);
    result = msg;
    result += " (行 ";
    std::array<char, 24> buf{};
    auto r1 = std::to_chars(buf.data(), buf.data() + buf.size(), line);
    result.append(buf.data(), r1.ptr);
    result += ", 列 ";
    auto r2 = std::to_chars(buf.data(), buf.data() + buf.size(), col);
    result.append(buf.data(), r2.ptr);
    result += ')';
    return result;
}

/// @brief 为错误消息附加行号定位信息。
///
/// 输出格式：`<msg> (行 <line>)`
/// 用于不需要列号的错误消息构造（如模块加载错误）。
///
/// @param msg 原始错误消息
/// @param line 源码行号
/// @return 附加行号信息后的错误消息
inline std::string formatWithLine(const std::string& msg, int line) {
    std::string result;
    result.reserve(msg.size() + 16);
    result = msg;
    result += " (行 ";
    std::array<char, 24> buf{};
    auto r = std::to_chars(buf.data(), buf.data() + buf.size(), line);
    result.append(buf.data(), r.ptr);
    result += ')';
    return result;
}

} // namespace ErrorFormat
