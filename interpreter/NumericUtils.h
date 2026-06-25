#pragma once

// ============================================================
// NumericUtils.h — 数值运算工具（跨模块共享）
// ------------------------------------------------------------
// 消除 Interpreter / VM / Compiler 三处重复的数值运算逻辑。
//
// 依赖：<cstdint> / <cmath>，不依赖 Value.h，无循环依赖风险。
// 全部声明为 constexpr，可供编译期常量折叠复用。
// ============================================================

#include <cstdint>
#include <cmath>
#include <string>
#include <type_traits>

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

// ============================================================
// C12 fix: 共享数值运算逻辑
// ------------------------------------------------------------
// Interpreter 与 VM 各自实现的算术/比较运算逻辑高度重复（~400 行）。
// 提取纯运算函数到此，错误处理仍由调用方按各自风格处理
// （Interpreter 抛 RuntimeError，VM 返回 VMResult 错误码）。
// ============================================================
namespace NumericOps {

/// 算术运算类型枚举（与 BinOpType 解耦，便于 VM 复用）
enum class ArithOp { Add, Sub, Mul, Div, Mod };

/// 算术运算结果状态
enum class ArithStatus {
    OK,           ///< 运算成功
    DivByZero,    ///< 除零
    IntOverflow,  ///< 整数溢出
    NotNumeric    ///< 操作数非数值（字符串拼接由调用方预先处理）
};

/// 算术运算结果
struct ArithResult {
    ArithStatus status = ArithStatus::OK;
    int64_t intVal = 0;     ///< 当两操作数均为 int 时有效
    double floatVal = 0.0;  ///< 当任一操作数为 float 时有效
    bool isIntResult = true; ///< true=使用 intVal，false=使用 floatVal

    bool ok() const { return status == ArithStatus::OK; }
};

/// 执行整数/浮点算术运算（不含字符串拼接，调用方预先处理）
/// 输入：两操作数已确认为数值类型（isNumber()=true）
inline ArithResult computeArith(ArithOp op, bool leftIsInt, int64_t leftInt,
                                double leftFloat, bool rightIsInt, int64_t rightInt,
                                double rightFloat) {
    ArithResult r;
    r.isIntResult = leftIsInt && rightIsInt;

    switch (op) {
    case ArithOp::Add:
        if (r.isIntResult) {
            if (OverflowCheck::addOverflow(leftInt, rightInt)) { r.status = ArithStatus::IntOverflow; return r; }
            r.intVal = leftInt + rightInt;
        } else {
            r.floatVal = leftFloat + rightFloat;
        }
        return r;
    case ArithOp::Sub:
        if (r.isIntResult) {
            if (OverflowCheck::subOverflow(leftInt, rightInt)) { r.status = ArithStatus::IntOverflow; return r; }
            r.intVal = leftInt - rightInt;
        } else {
            r.floatVal = leftFloat - rightFloat;
        }
        return r;
    case ArithOp::Mul:
        if (r.isIntResult) {
            if (OverflowCheck::mulOverflow(leftInt, rightInt)) { r.status = ArithStatus::IntOverflow; return r; }
            r.intVal = leftInt * rightInt;
        } else {
            r.floatVal = leftFloat * rightFloat;
        }
        return r;
    case ArithOp::Div:
        if (r.isIntResult) {
            if (rightInt == 0) { r.status = ArithStatus::DivByZero; return r; }
            if (OverflowCheck::divOverflow(leftInt, rightInt)) { r.status = ArithStatus::IntOverflow; return r; }
            r.intVal = leftInt / rightInt;  // int/int → int (截断除法)
        } else {
            if (rightFloat == 0.0) { r.status = ArithStatus::DivByZero; return r; }
            r.floatVal = leftFloat / rightFloat;
        }
        return r;
    case ArithOp::Mod:
        if (r.isIntResult) {
            if (rightInt == 0) { r.status = ArithStatus::DivByZero; return r; }
            if (OverflowCheck::modOverflow(leftInt, rightInt)) { r.status = ArithStatus::IntOverflow; return r; }
            r.intVal = leftInt % rightInt;
        } else {
            if (rightFloat == 0.0) { r.status = ArithStatus::DivByZero; return r; }
            r.floatVal = std::fmod(leftFloat, rightFloat);
        }
        return r;
    }
    r.status = ArithStatus::NotNumeric;
    return r;
}

/// 有序比较类型
enum class CompareOp { Less, Greater, LessEqual, GreaterEqual };

/// 有序比较结果状态
enum class CompareStatus {
    OK,           ///< 比较成功
    NotComparable ///< 操作数类型不匹配（非数值或非字符串对）
};

/// 有序比较结果
struct CompareResult {
    CompareStatus status = CompareStatus::OK;
    bool value = false;

    bool ok() const { return status == CompareStatus::OK; }
};

/// 执行有序比较（字符串字典序或数值比较）
/// 输入：leftIsString/rightIsString 标识类型，leftNum/rightNum 为数值（当非字符串时）
inline CompareResult computeCompare(CompareOp op, bool leftIsString, const std::string& leftStr,
                                    bool rightIsString, const std::string& rightStr,
                                    double leftNum, double rightNum) {
    CompareResult r;
    // 字符串字典序比较（两侧均为字符串）
    if (leftIsString && rightIsString) {
        switch (op) {
        case CompareOp::Less:        r.value = leftStr <  rightStr; break;
        case CompareOp::Greater:     r.value = leftStr >  rightStr; break;
        case CompareOp::LessEqual:   r.value = leftStr <= rightStr; break;
        case CompareOp::GreaterEqual:r.value = leftStr >= rightStr; break;
        }
        return r;
    }
    // 数值比较（调用方已确认两侧均为数值）
    switch (op) {
    case CompareOp::Less:        r.value = leftNum <  rightNum; break;
    case CompareOp::Greater:     r.value = leftNum >  rightNum; break;
    case CompareOp::LessEqual:   r.value = leftNum <= rightNum; break;
    case CompareOp::GreaterEqual:r.value = leftNum >= rightNum; break;
    }
    return r;
}

} // namespace NumericOps

