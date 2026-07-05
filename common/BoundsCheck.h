/**
 * @file common/BoundsCheck.h
 * @brief 共享的索引越界检查工具。
 *
 * Dedup-7A fix: 提取分散在 VMContainers.cpp（7 处）和 Interpreter.cpp（1 处）
 * 中的 `i >= 0 && static_cast<size_t>(i) < container.size()` 重复模式。
 * 消除每处 ~50 字节的样板，统一负索引与溢出语义。
 *
 * @see BoundsCheck::inBounds
 */
#pragma once

// ============================================================
// BoundsCheck.h — 共享的索引越界检查工具
// ------------------------------------------------------------
// Dedup-7A fix: 提取分散在 VMContainers.cpp (7 处) 和 Interpreter.cpp (1 处)
// 中的 `i >= 0 && static_cast<size_t>(i) < container.size()` 重复模式。
// 消除每处 ~50 字节的样板，统一负索引与溢出语义。
// ============================================================

#include <cstdint>
#include <cstddef>

namespace BoundsCheck {

/// 检查 int64_t 索引是否落在 [0, size) 范围内。
/// 负索引返回 false（不发生符号扩展为巨大的 size_t）。
/// @param index 待检查索引
/// @param size  容器长度
inline bool inBounds(int64_t index, size_t size) {
    return index >= 0 && static_cast<size_t>(index) < size;
}

} // namespace BoundsCheck
