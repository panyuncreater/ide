// ============================================================
// TestL14RuntimeErrorCatch.cpp - L14: try/catch 捕获 runtimeError
// ============================================================
// 验证四后端（Interpreter / StackVM / RegisterVM / JIT）一致地
// 将运行时错误（如除零、索引越界）转换为可被 try/catch 捕获的异常。
//
// L14 语义变更：
//   - 原：try/catch 仅捕获显式 throw 语句，不捕获 runtimeError
//   - 新：try/catch 同时捕获显式 throw 和 runtimeError（除零等）
//   - 异常值为错误消息字符串（与 throw "msg" 语义一致）
// ============================================================

#include "common/pch.h"
#include "compiler/Compiler.h"
#include "compiler/JIT.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>

namespace {

std::string runInterpreter(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors())
        return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        // L14: 未捕获的 RuntimeError 包含错误消息
        if (out.empty())
            return std::string(e.what());
        return out + std::string(e.what());
    } catch (const ThrowException& e) {
        if (out.empty())
            return e.thrownValue.toString();
        return out + e.thrownValue.toString();
    }
    return out;
}

std::string runStackVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors())
        return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile-fail>";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty())
            return vm.getLastError();
        return out + vm.getLastError();
    }
    return out;
}

std::string runRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors())
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile-fail>";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

std::string runJIT(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors())
        return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile-fail>";
    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    JitResult result = jit.execute(cr);
    if (result == JitResult::CompileError)
        return out + "<jit-compile>";
    if (result == JitResult::RuntimeError) {
        std::string err = jit.getLastError();
        if (out.empty())
            return err;
        return out + err;
    }
    return out;
}

} // namespace

// ============================================================
// L14 测试：try/catch 捕获除零错误（四后端一致性）
// ============================================================

// 除零被 try/catch 捕获，catch 变量包含错误消息
TEST(L14RuntimeErrorCatch, DivisionByZeroCaught) {
    std::string src = "try { var x = 1 / 0; print(\"no-print\"); } catch (e) { print(e); }";
    std::string expected = "除零错误";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
    EXPECT_EQ(runJIT(src), expected);
}

// 取模除零被 try/catch 捕获
TEST(L14RuntimeErrorCatch, ModuloByZeroCaught) {
    std::string src = "try { var x = 10 % 0; print(\"no-print\"); } catch (e) { print(e); }";
    std::string expected = "除零错误";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
    EXPECT_EQ(runJIT(src), expected);
}

// 浮点除零（通过 C++ 辅助路径）
TEST(L14RuntimeErrorCatch, FloatDivisionByZeroCaught) {
    std::string src = "try { var x = 1.0 / 0.0; print(\"no-print\"); } catch (e) { print(e); }";
    std::string expected = "除零错误";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
    // JIT 浮点除零走 C++ 辅助路径（jitDivGeneric），通过 jitCheckAndRethrow 转异常
    EXPECT_EQ(runJIT(src), expected);
}

// 除零在函数内，catch 在外层
TEST(L14RuntimeErrorCatch, DivisionByZeroInFunctionCaughtByCaller) {
    std::string src = "fun div(a, b) { return a / b; }"
                      "try { print(div(10, 0)); } catch (e) { print(e); }";
    std::string expected = "除零错误";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
    EXPECT_EQ(runJIT(src), expected);
}

// try 内未发生错误时正常执行
TEST(L14RuntimeErrorCatch, NoErrorNormalExecution) {
    std::string src = "try { var x = 10 / 2; print(x); } catch (e) { print(e); }";
    std::string expected = "5";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
    EXPECT_EQ(runJIT(src), expected);
}

// 除零错误未被 try/catch 捕获时正常报错
TEST(L14RuntimeErrorCatch, UncaughtDivisionByZeroStillErrors) {
    std::string src = "var x = 1 / 0;";
    // Interpreter 抛出 RuntimeError（含错误消息）
    EXPECT_NE(runInterpreter(src).find("除零错误"), std::string::npos);
    // StackVM/RegisterVM/JIT 返回错误
    EXPECT_NE(runStackVM(src).find("除零错误"), std::string::npos);
    EXPECT_NE(runRegVM(src).find("除零错误"), std::string::npos);
    EXPECT_NE(runJIT(src).find("除零错误"), std::string::npos);
}

// catch 后继续执行后续代码
TEST(L14RuntimeErrorCatch, ExecutionContinuesAfterCatch) {
    std::string src = "try { var x = 1 / 0; } catch (e) { print(\"caught\"); }"
                      "print(\"after\");";
    std::string expected = "caughtafter";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
    EXPECT_EQ(runJIT(src), expected);
}

// finally 块在 runtimeError 被捕获后仍然执行
TEST(L14RuntimeErrorCatch, FinallyRunsAfterRuntimeErrorCaught) {
    std::string src = "try { var x = 1 / 0; } catch (e) { print(\"catch\"); }"
                      "finally { print(\"finally\"); }";
    std::string expected = "catchfinally";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
    EXPECT_EQ(runJIT(src), expected);
}
