/**
 * @file common/Utf8Utils.h
 * @brief UTF-8 码位工具函数（内联，无依赖）。
 *
 * 提取自 BuiltinMethods.cpp / VM.cpp 中 5 处重复的 UTF-8 首字节解码逻辑。
 * 所有函数对非法字节序列采取"跳过 1 字节"策略（返回 1），与原实现一致。
 *
 * 提供的函数：
 *   - byteLength(c)：UTF-8 首字节 → 码位字节数（1-4）
 *   - codepointCount(s)：统计字符串中的 UTF-8 码位数
 *   - codepointToByteIndex(s, cpIdx)：码位索引 → 字节索引
 *   - byteToCodepointIndex(s, byteIdx)：字节索引 → 码位索引
 *
 * 用法示例：
 * @code
 *   std::string s = "你好";
 *   int64_t cps = Utf8::codepointCount(s);  // = 2
 *   size_t byteIdx = Utf8::codepointToByteIndex(s, 1);  // = 3
 * @endcode
 *
 * @see Value::codepointCount()
 */
#pragma once

// ============================================================
// Utf8Utils.h — UTF-8 码位工具函数（内联，无依赖）
// ============================================================
// 提取自 BuiltinMethods.cpp / VM.cpp 中 5 处重复的 UTF-8 首字节解码逻辑。
// 所有函数对非法字节序列采取"跳过 1 字节"策略（返回 1），与原实现一致。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>

namespace Utf8 {

/// 返回 UTF-8 首字节对应的码位字节数（1-4），非法首字节返回 1（跳过）
inline int byteLength(unsigned char c) {
    return (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
}

/// 统计字符串中的 UTF-8 码位数
inline int64_t codepointCount(const std::string& s) {
    int64_t count = 0;
    for (size_t i = 0; i < s.size();) {
        i += byteLength(static_cast<unsigned char>(s[i]));
        count++;
    }
    return count;
}

/// 将码位索引转换为字节索引。cpIdx 超出范围时返回 s.size()。
inline size_t codepointToByteIndex(const std::string& s, int64_t cpIdx) {
    size_t bytePos = 0;
    int64_t cp = 0;
    while (bytePos < s.size() && cp < cpIdx) {
        bytePos += byteLength(static_cast<unsigned char>(s[bytePos]));
        cp++;
    }
    // AUDIT-BUG-C4 fix: 钳制到 s.size()——byteLength 对不完整 UTF-8 序列（如 4 字节
    // leader 仅剩 1-3 字节）返回 4，可将 bytePos 推进到 s.size() 之外。
    // 调用方（如 substr）若直接用作索引会越界抛 std::out_of_range。
    if (bytePos > s.size())
        bytePos = s.size();
    return bytePos;
}

/// 将字节索引转换为码位索引（计算前 byteIdx 字节包含多少码位）。
inline int64_t byteToCodepointIndex(const std::string& s, size_t byteIdx) {
    int64_t charIdx = 0;
    for (size_t b = 0; b < byteIdx;) {
        b += byteLength(static_cast<unsigned char>(s[b]));
        charIdx++;
    }
    return charIdx;
}

} // namespace Utf8
