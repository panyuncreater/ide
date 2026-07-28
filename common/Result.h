/**
 * @file common/Result.h
 * @brief 统一错误处理模板 Result<T>。
 *
 * S6 fix: 替代项目中分散的 SharedBuiltinResult / BuiltinMethodResult /
 * VMResult 等特化结构体，提供统一的成功值/错误值承载类型。
 *
 * 设计要点：
 *   - 持有成功值 T 或错误 ErrorInfo，不抛异常
 *   - 错误信息含 message + line + column，与 RuntimeError 对齐
 *   - 提供 ok() / err() 工厂函数，is_ok() / is_err() 查询
 *   - value() / error() 访问器（不安全，调用方需先检查）
 *   - unwrap_or() 提供默认值回退
 *
 * 依赖约束：common 层不反向依赖 interpreter 层。to_runtime_error()
 * 转换由调用方所在的 interpreter 层自行实现（见 RuntimeExceptions.h
 * 末尾的自由函数模板 to_runtime_error(const Result<T>&)）。
 *
 * 用法示例：
 * @code
 *   Result<int> parseInt(const std::string& s) {
 *       if (s.empty()) return Result<int>::err("空字符串");
 *       return Result<int>::ok(std::stoi(s));
 *   }
 *   auto r = parseInt("42");
 *   if (r.is_ok()) use(r.value());
 *   else log(r.error().message);
 * @endcode
 *
 * @see ErrorInfo RuntimeExceptions.h
 */
#pragma once

// ============================================================
// Result.h - 统一错误处理模板
// ============================================================
// S6 fix: 统一错误处理模式，替代项目中分散的 SharedBuiltinResult /
// BuiltinMethodResult / VMResult 等特化结构体。
//
// A1 fix: common 层不再反向依赖 interpreter 层。
// Result.h 仅保留泛型 Result<T> 框架与 ErrorInfo，不包含
// RuntimeExceptions.h。to_runtime_error() 转换由调用方所在的
// interpreter 层自行实现（见 RuntimeExceptions.h 末尾的自由函数模板
// to_runtime_error(const Result<T>&)）。
//
// 设计要点：
//   1. Result<T> 持有成功值或错误信息，不抛异常
//   2. 错误信息包含 message + line + column，与 RuntimeError 对齐
//   3. 提供 ok() / err() 工厂函数，is_ok() / is_err() 查询
//   4. value() / error() 访问器（不安全，调用方需先检查）
//   5. unwrap_or() 提供默认值回退
// ============================================================

#include <string>
#include <utility>
#include <variant>

/// 错误信息载体（与 RuntimeError 字段对齐）
struct ErrorInfo {
    std::string message;
    int line = 0;
    int column = 0;

    ErrorInfo() = default;
    ErrorInfo(std::string msg, int ln = 0, int col = 0) : message(std::move(msg)), line(ln), column(col) {}
};

/// 统一结果类型：持有成功值 T 或错误 ErrorInfo
/// AUDIT-R4 BUG-08 fix: 标注 [[nodiscard]]——调用方丢弃返回的 Result 即静默
/// 吞错误（无任何诊断）。加标注后编译器在 /W4+/WX 下强制所有调用点
/// 显式处理或显式丢弃（(void) 转换）。
template <typename T> class [[nodiscard]] Result {
public:
    /// 成功构造
    /// AUDIT-R3 P2 fix: 用 in_place_index 直接构造 variant，消除"先默认构造
    /// Result 再 emplace"的模式——原实现隐式要求 T 可默认构造（variant 默认
    /// 构造第 0 备选 T），且 err 路径白白构造再销毁一个 T。
    static Result ok(T value) { return Result(std::variant<T, ErrorInfo>(std::in_place_index<0>, std::move(value))); }

    /// 错误构造
    static Result err(ErrorInfo error) {
        return Result(std::variant<T, ErrorInfo>(std::in_place_index<1>, std::move(error)));
    }

    /// 错误构造（便捷版）
    static Result err(std::string message, int line = 0, int column = 0) {
        return err(ErrorInfo(std::move(message), line, column));
    }

    bool is_ok() const { return data_.index() == 0; }
    bool is_err() const { return data_.index() == 1; }

    /// 获取成功值（仅 is_ok() 时调用）
    T& value() { return std::get<0>(data_); }
    const T& value() const { return std::get<0>(data_); }

    /// 获取错误信息（仅 is_err() 时调用）
    const ErrorInfo& error() const { return std::get<1>(data_); }

    /// 获取成功值或默认值
    /// AUDIT-R3 P2 fix: 改为 if/else——原三目运算符混合 const 左值与右值，
    /// 公共类型退化为纯右值导致 fallback 分支也多一次拷贝；现 fallback
    /// 分支走函数参数的隐式 move。
    T unwrap_or(T fallback) const {
        if (is_ok())
            return std::get<0>(data_);
        return fallback;
    }

private:
    /// AUDIT-R3 P2 fix: 私有 variant 构造（供 ok/err 工厂使用，不要求 T 可默认构造）
    explicit Result(std::variant<T, ErrorInfo>&& v) : data_(std::move(v)) {}

    std::variant<T, ErrorInfo> data_;
};

// A1 fix: to_runtime_error() 移至 interpreter/RuntimeExceptions.h 作为自由函数模板，
// 由调用方（interpreter 层）包含。common 层保持无依赖。
