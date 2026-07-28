#pragma once

// ============================================================
// tests/TestJITHelper.h — JIT 测试共享辅助函数
// ------------------------------------------------------------
// 从 TestJIT.cpp 拆分，供所有 TestJIT*.cpp 文件共享。
// 提供 runJIT() 辅助函数：编译 MiniLang 源码并通过 JIT 后端执行，
// 收集输出字符串或错误标记。
// ============================================================

#include "common/ThreeBackends.h"
#include "compiler/Compiler.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#ifdef MINILANG_USE_JIT
#include "compiler/JIT.h"
#include "interpreter/GcManager.h" // P2-9: CallbackSuppressor 测试
#endif

#include <gtest/gtest.h>
#include <string>

#ifdef MINILANG_USE_JIT

namespace jit_test_helper {

/// 运行 JIT 后端，返回输出字符串。
/// 失败时返回带错误标记的字符串（与 ThreeBackends.h 风格一致）。
inline std::string runJIT(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";

    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    JitResult result = jit.execute(cr);
    if (result == JitResult::CompileError)
        return out + "<jit-compile:" + jit.getLastError() + ">";
    if (result == JitResult::RuntimeError)
        return out + "<jit-runtime:" + jit.getLastError() + ">";
    return out;
}

} // namespace jit_test_helper

// 便捷宏：在测试中直接使用 runJIT 而不需要 namespace 前缀
using jit_test_helper::runJIT;

#endif // MINILANG_USE_JIT
