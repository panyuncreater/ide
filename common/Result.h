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
    ErrorInfo(std::string msg, int ln = 0, int col = 0)
        : message(std::move(msg)), line(ln), column(col) {}
};

/// 统一结果类型：持有成功值 T 或错误 ErrorInfo
template<typename T>
class Result {
public:
    /// 成功构造
    static Result ok(T value) {
        Result r;
        r.data_.template emplace<0>(std::move(value));
        return r;
    }

    /// 错误构造
    static Result err(ErrorInfo error) {
        Result r;
        r.data_.template emplace<1>(std::move(error));
        return r;
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
    T unwrap_or(T fallback) const {
        return is_ok() ? std::get<0>(data_) : std::move(fallback);
    }

private:
    std::variant<T, ErrorInfo> data_;
};

// A1 fix: to_runtime_error() 移至 interpreter/RuntimeExceptions.h 作为自由函数模板，
// 由调用方（interpreter 层）包含。common 层保持无依赖。
