#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/// OPT-10: 字节码行/列元数据紧凑存储。
/// 原实现为 lines + columns 两个 vector<int>，每字节 8B；执行期仅错误/调试路径读取。
/// 现打包为每字节 4B（高 16 位行号、低 16 位列号，超出范围饱和截断）。
/// 源文件行号 > 65535 或列号 > 65535 属于极端情况，诊断定位饱和到最大值。
struct BytecodeLineColumnMap {
    std::vector<uint32_t> entries;

    static uint32_t pack(int line, int column) noexcept {
        const uint32_t l = line <= 0 ? 0u : (line >= 0xFFFF ? 0xFFFFu : static_cast<uint32_t>(line));
        const uint32_t c = column <= 0 ? 0u : (column >= 0xFFFF ? 0xFFFFu : static_cast<uint32_t>(column));
        return (l << 16) | c;
    }

    void push(int line, int column) { entries.push_back(pack(line, column)); }
    void reserve(size_t n) { entries.reserve(n); }
    void clear() { entries.clear(); }
    void resize(size_t n) { entries.resize(n); }
    size_t size() const { return entries.size(); }
    bool empty() const { return entries.empty(); }
    int lineAt(size_t offset) const { return offset < entries.size() ? static_cast<int>(entries[offset] >> 16) : 0; }
    int columnAt(size_t offset) const {
        return offset < entries.size() ? static_cast<int>(entries[offset] & 0xFFFFu) : 0;
    }
};
