#pragma once

// ============================================================
// NumericUtils.h — 整数算术溢出检查工具（跨模块共享）
// ------------------------------------------------------------
// 消除 Interpreter / VM / Compiler 三处重复的溢出判定逻辑，
// 统一修复 Interpreter.cpp 浮点转整数检查的 Bug（> 应为 >=）。
//
// 依赖：<cstdint> / <cmath>，不依赖 Value.h，无循环依赖风险。
// 全部声明为 constexpr，可供编译期常量折叠复用。
// ============================================================

#include <cstdint>
#include <cmath>

namespace OverflowCheck {

/// 加法 a + b 是否溢出 int64_t
constexpr bool addOverflow(int64_t a, int64_t b) {
    return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

/// 减法 a - b 是否溢出 int64_t
constexpr bool subOverflow(int64_t a, int64_t b) {
    return (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
}

/// 乘法 a * b 是否溢出 int64_t（内部已处理 -1 * INT64_MIN）
constexpr bool mulOverflow(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return false;
    if (a == -1 && b == INT64_MIN) return true;
    if (b == -1 && a == INT64_MIN) return true;
    return (a > 0 && b > 0 && a > INT64_MAX / b) ||
           (a > 0 && b < 0 && b < INT64_MIN / a) ||
           (a < 0 && b > 0 && a < INT64_MIN / b) ||
           (a < 0 && b < 0 && a < INT64_MAX / b);
}

/// 除法 a / b 是否为 UB（仅 INT64_MIN / -1；b == 0 由调用方单独处理）
constexpr bool divOverflow(int64_t a, int64_t b) {
    return a == INT64_MIN && b == -1;
}

/// 取模 a % b 是否为 UB（仅 INT64_MIN % -1；b == 0 由调用方单独处理）
constexpr bool modOverflow(int64_t a, int64_t b) {
    return a == INT64_MIN && b == -1;
}

/// 一元取负 -a 是否为 UB（仅 -INT64_MIN）
constexpr bool negateOverflow(int64_t a) {
    return a == INT64_MIN;
}

/// double 转 int64_t 是否溢出
/// 注意：使用 >= 上界，因 double(INT64_MAX) 上取整为 2^63，
/// 该值无法表示为 int64_t（M-新3 fix，统一修复 Interpreter.cpp 的旧版 > Bug）
inline bool doubleToIntOverflow(double d) {
    return d < static_cast<double>(INT64_MIN) || d >= -static_cast<double>(INT64_MIN);
}

} // namespace OverflowCheck
