#pragma once

// ============================================================
// RuntimeExceptions.h - 解释器运行时异常与调用帧
// ============================================================
// S6 fix: 从 Interpreter.h 提取，降低头文件耦合。
// 仅依赖 Value.h 和标准库，不依赖 AST/Visitor/Environment。
// 需要 RuntimeError/BreakException 等但不依赖 Interpreter 类的模块
// 可直接包含此头文件，避免拉入 AST/Visitor 等重型依赖。
//
// A1 fix: 新增自由函数模板 to_runtime_error(const Result<T>&)，
// 将原 Result<T>::to_runtime_error() 成员函数迁移至此，
// 使 common/Result.h 不再反向依赖 interpreter 层。
// ============================================================

#include <string>
#include <stdexcept>
#include <memory>
#include "interpreter/Value.h"
#include "interpreter/Environment.h"  // CallFrame 需要 shared_ptr<Environment>
#include "common/Result.h"  // A1 fix: to_runtime_error 自由函数模板需要 Result<T>

// ============================================================
// 运行时异常
// ============================================================

/// 运行时错误
class RuntimeError : public std::runtime_error {
public:
    int line;
    int column;

    RuntimeError(const std::string& msg, int ln = 0, int col = 0)
        : std::runtime_error(msg), line(ln), column(col) {}
};

/// return 语句专用异常（用于跳出函数体）
class ReturnException : public std::runtime_error {
public:
    Value returnValue;

    ReturnException(Value val)
        : std::runtime_error("return"), returnValue(std::move(val)) {}
};

/// break 语句专用异常（用于跳出循环体）
class BreakException : public std::runtime_error {
public:
    BreakException() : std::runtime_error("break") {}
};

/// continue 语句专用异常（用于跳到循环下一次迭代）
class ContinueException : public std::runtime_error {
public:
    ContinueException() : std::runtime_error("continue") {}
};

/// throw 语句专用异常（用于 try/catch 捕获）
class ThrowException : public std::runtime_error {
public:
    Value thrownValue;

    ThrowException(Value val)
        : std::runtime_error("throw"), thrownValue(std::move(val)) {}
};

/// 调试终止异常（用户点击停止按钮时抛出）
class DebugStopException : public std::exception {
public:
    const char* what() const noexcept override { return "调试终止"; }
};

// ============================================================
// 调用帧
// ============================================================

/// 函数调用帧
struct CallFrame {
    std::string functionName;                      // 函数名
    std::shared_ptr<Environment> env = nullptr;     // 该帧对应的环境
    int line = 0;                                   // 调用行号
    int depth = 0;                                  // 调用深度

    CallFrame() = default;
    CallFrame(const std::string& name, std::shared_ptr<Environment> e, int ln, int d)
        : functionName(name), env(e), line(ln), depth(d) {}
};

// ============================================================
// A1 fix: Result → RuntimeError 转换（自由函数模板）
// ============================================================
// 原 Result<T>::to_runtime_error() 成员函数需要 Result.h 包含
// RuntimeExceptions.h，导致 common 层反向依赖 interpreter 层。
// 现迁移为自由函数模板，定义在 interpreter 层，由调用方包含此头文件使用。
// 调用方需确保 result.is_err()。

template<typename T>
RuntimeError to_runtime_error(const Result<T>& result) {
    const auto& e = result.error();
    return RuntimeError(e.message, e.line, e.column);
}
