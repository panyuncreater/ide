#pragma once

// ============================================================
// RuntimeExceptions.h - 解释器运行时异常
// ============================================================
// S6 fix: 从 Interpreter.h 提取，降低头文件耦合。
// 仅依赖 Value.h 和标准库，不依赖 AST/Visitor/Environment。
// 需要 RuntimeError/BreakException 等但不依赖 Interpreter 类的模块
// 可直接包含此头文件，避免拉入 AST/Visitor 等重型依赖。
//
// A1 fix: 新增自由函数模板 to_runtime_error(const Result<T>&)，
// 将原 Result<T>::to_runtime_error() 成员函数迁移至此，
// 使 common/Result.h 不再反向依赖 interpreter 层。
//
// ARCH-02 fix: CallFrame 拆出到独立头文件 CallFrame.h。
// 原本因 CallFrame 需要 shared_ptr<Environment> 而 include Environment.h，
// 传递拉入 Environment/Value 类型体系。现仅依赖 Value.h（异常类持有 Value）。
// 真正使用 CallFrame 的模块（Interpreter.h）请 include "interpreter/CallFrame.h"。
// ============================================================

#include "common/Result.h" // A1 fix: to_runtime_error 自由函数模板需要 Result<T>
#include "interpreter/Value.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <vector> // B1 TCO: TailCallSignal 携带实参列表

// ============================================================
// 运行时异常
// ============================================================

/// 运行时错误
class RuntimeError : public std::runtime_error {
public:
    int line;
    int column;
    // P2 fix (错误码优先匹配): 稳定诊断码，如 "division-by-zero"。
    // 与 ErrorHintEngine::errorPatterns() 表中的 tag 对应。空字符串表示未设置，
    // ErrorHintEngine 回退到中/英子串匹配兜底逻辑。由 Interpreter::runtimeError
    // 在抛出时填充，runStatementsWithExceptionHandling 的 catch 块透传到 addError。
    std::string code;

    RuntimeError(const std::string& msg, int ln = 0, int col = 0) : std::runtime_error(msg), line(ln), column(col) {}

    /// P2 fix: 带 code 的构造重载（引擎迁移时使用）
    RuntimeError(const std::string& msg, int ln, int col, const std::string& diagCode)
        : std::runtime_error(msg), line(ln), column(col), code(diagCode) {}
};

/// return 语句专用异常（用于跳出函数体）
class ReturnException : public std::runtime_error {
public:
    Value returnValue;

    ReturnException(Value val) : std::runtime_error("return"), returnValue(std::move(val)) {}
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

    ThrowException(Value val) : std::runtime_error("throw"), thrownValue(std::move(val)) {}
};

/// R164 协程/生成器：yield 信号异常（重放模式下从函数体抛出，被 .next() 拦截）
/// 语义：visitYieldExpr 在命中目标 yieldId 时抛出，携带 yield 表达式的值。
/// .next() 调用方捕获此异常，递增 currentYieldId 并返回值。
class YieldSignal : public std::runtime_error {
public:
    Value yieldValue;

    explicit YieldSignal(Value val) : std::runtime_error("yield"), yieldValue(std::move(val)) {}
};

/// B1 TCO：自尾调用信号——visitReturnStmt 识别 `return f(args)` /
/// `return this.m(args)` 自尾调用后抛出（携带已求值的实参），由
/// callNamedFunction / invokeMethod 的蹦床循环捕获并帧复用执行，
/// 使深尾递归在恒定 C++ 栈深内完成（对齐 StackVM/RegisterVM 的 TCO）。
/// 不是错误：仅在 TCO 上下文启用（tcoEnabled_）且不在 try 块内时抛出，
/// 非蹦床的函数体执行点（init/生成器/闭包变量/高阶回调）显式禁用上下文，
/// 保证信号不会逃逸出对应的蹦床边界。
class TailCallSignal : public std::runtime_error {
public:
    std::vector<Value> args;
    // L18 eng-tailcall: 互递归目标。非空时蹦床循环切换到目标 FunDecl；
    // targetClosure 携带目标闭包值（浅拷贝共享 ClosureData），蹦床用其
    // closureEnv/capturedVars 重建目标环境（复用现有 rebuild 机制）。
    // 空 = 自递归（原语义不变）。
    std::shared_ptr<FunDecl> target;
    std::string targetName;
    Value targetClosure;

    explicit TailCallSignal(std::vector<Value> a) : std::runtime_error("tailcall"), args(std::move(a)) {}
    TailCallSignal(std::vector<Value> a, std::shared_ptr<FunDecl> t, std::string tn, Value tc)
        : std::runtime_error("tailcall"), args(std::move(a)), target(std::move(t)), targetName(std::move(tn)),
          targetClosure(std::move(tc)) {}
};

/// 调试终止异常（用户点击停止按钮时抛出）
class DebugStopException : public std::exception {
public:
    const char* what() const noexcept override { return "调试终止"; }
};

// ============================================================
// A1 fix: Result → RuntimeError 转换（自由函数模板）
// ============================================================
// 原 Result<T>::to_runtime_error() 成员函数需要 Result.h 包含
// RuntimeExceptions.h，导致 common 层反向依赖 interpreter 层。
// 现迁移为自由函数模板，定义在 interpreter 层，由调用方包含此头文件使用。
// 调用方需确保 result.is_err()。

template <typename T> RuntimeError to_runtime_error(const Result<T>& result) {
    const auto& e = result.error();
    return RuntimeError(e.message, e.line, e.column);
}
