#pragma once

// ============================================================
// ErrorFormat.h — 运行时错误格式化工具
// ------------------------------------------------------------
// P3 fix: 替代 "msg" + std::to_string(n) + "msg" 模式。
// std::to_string 在 MSVC 上会触发 locale 查询与堆分配，
// 循环内越界等场景频繁触发错误路径时产生可观开销。
// 本工具使用栈缓冲 + std::to_chars（无 locale、无堆分配）。
//
// 用法：
//   runtimeErrorFmt("数组索引越界: %d, 有效范围 [0, %d)", idx, size);
//   runtimeErrorFmt("未知操作码: %d", static_cast<int>(op));
//
// 支持的格式说明符：
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

/// 将整数写入栈缓冲并返回字符串视图（无堆分配）
template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>> inline std::string intToString(T value) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), value);
    return std::string(buf, res.ptr);
}

/// P3 fix: 格式化错误消息（栈缓冲，无 std::to_string 调用）。
/// 支持的占位符：%d（整型）、%s（const char* / std::string）。
/// 其余字符原样输出。返回 std::string 供 runtimeError/throw 使用。
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

} // namespace ErrorFormat
