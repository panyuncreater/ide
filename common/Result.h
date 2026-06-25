#pragma once

// ============================================================
// Result.h - 统一错误处理模板
// ============================================================
// S6 fix: 统一错误处理模式，替代项目中分散的 SharedBuiltinResult /
// BuiltinMethodResult / VMResult 等特化结构体。
//
// 设计要点：
//   1. Result<T> 持有成功值或错误信息，不抛异常
//   2. 错误信息包含 message + line + column，与 RuntimeError 对齐
//   3. 提供 ok() / err() 工厂函数，is_ok() / is_err() 查询
//   4. value() / error() 访问器（不安全，调用方需先检查）
//   5. unwrap_or() 提供默认值回退
//
// 迁移策略：
//   - 第一阶段：SharedBuiltinResult → Result<Value>（共享层）
//   - 第二阶段：VMResult → Result<void>（VM 层，后续）
//   - 第三阶段：Interpreter 异常路径保持不变（81 个 throw 站点迁移成本过高）
// ============================================================

#include <string>
#include <utility>
#include <variant>
#include "interpreter/RuntimeExceptions.h"  // P2-9 fix: to_runtime_error() 需要完整 RuntimeError 类型

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

    /// 转换为 RuntimeError（用于 Interpreter 侧从 Result 转回异常）
    /// 调用方需确保 is_err()
    class RuntimeError to_runtime_error() const;

private:
    std::variant<T, ErrorInfo> data_;
};

// P2-9 fix: RuntimeError 完整定义由 interpreter/RuntimeExceptions.h 提供（顶部包含）

template<typename T>
RuntimeError Result<T>::to_runtime_error() const {
    const auto& e = error();
    return RuntimeError(e.message, e.line, e.column);
}
