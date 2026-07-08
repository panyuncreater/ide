#pragma once

// ============================================================
// CallFrame.h - 解释器函数调用帧
// ============================================================
// ARCH-02 fix: 从 RuntimeExceptions.h 拆出，消除传递依赖。
// 原先 RuntimeExceptions.h 因 CallFrame 需要 shared_ptr<Environment>
// 而 include Environment.h，导致仅需要 RuntimeError/BreakException 等
// 异常类的模块（BuiltinMethods.h、DebugController.cpp、common/Result.h
// 使用方）被传递拉入整个 Environment/Value 类型体系。
//
// 现拆分后：RuntimeExceptions.h 只依赖 Value.h（异常类持有 Value），
// 仅 Interpreter.h 等真正使用 CallFrame 的模块 include 本头文件。
// ============================================================

#include "interpreter/Environment.h" // CallFrame 需要 shared_ptr<Environment>
#include <memory>
#include <string>

/// 函数调用帧
struct CallFrame {
    std::string functionName;                   // 函数名
    std::shared_ptr<Environment> env = nullptr; // 该帧对应的环境
    int line = 0;                               // 调用行号
    int depth = 0;                              // 调用深度

    CallFrame() = default;
    CallFrame(const std::string& name, std::shared_ptr<Environment> e, int ln, int d)
        : functionName(name), env(e), line(ln), depth(d) {}
};
