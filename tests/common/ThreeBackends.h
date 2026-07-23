// ============================================================
// tests/common/ThreeBackends.h
// ------------------------------------------------------------
// R133 引入：四后端一致性测试共享基础设施。
// 抽取自 tests/TestClosureAudit.cpp (R123) 中的 runInterpreter /
// runStackVM / runStackVM_IR / runRegVM 辅助函数 + EXPECT_ALL_BACKENDS 宏。
//
// 目的：避免每个新测试文件都重复实现四后端调用样板代码。
// 后续测试文件只需 #include "common/ThreeBackends.h" 即可使用。
//
// 使用方式：
//   #include "common/ThreeBackends.h"
//   TEST(Foo, Bar) {
//       EXPECT_ALL_BACKENDS("print(1+2);", "3");
//   }
//
// 注意：本头文件包含完整的函数定义（inline），多个翻译单元包含
// 不会产生 ODR 冲突。函数故意标记为 static 以限制可见性到当前 TU,
// 避免链接器符号冲突。
// ============================================================
#pragma once

#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>
#include <string>

#ifdef MINILANG_USE_JIT
#include "compiler/JIT.h"
#endif

namespace minilang_test {

// 运行 Interpreter 树遍历后端。失败时返回包含错误标记的字符串,
// 与其他 run* 函数返回格式一致，便于 EXPECT_ALL_BACKENDS 直接比较。
inline std::string runInterpreter(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

// 运行 StackVM（非 IR 路径）。
inline std::string runStackVM(const std::string& src) {
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
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

// 运行 StackVM（IR 路径：AstIRBuilder → BytecodeIRBackend → VM）。
inline std::string runStackVM_IR(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

// 运行 RegisterVM（IR → RegisterBytecodeBackend → RegisterVM）。
inline std::string runRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

} // namespace minilang_test

// 验证四后端一致：所有后端输出必须等于 expected。
// 引入 minilang_test 命名空间避免与各测试文件本地 run* 函数冲突。
// 测试文件如已存在同名本地函数，应删除以避免歧义。
#define EXPECT_ALL_BACKENDS(src, expected)                                                                             \
    EXPECT_EQ(minilang_test::runInterpreter(src), expected);                                                           \
    EXPECT_EQ(minilang_test::runStackVM(src), expected);                                                               \
    EXPECT_EQ(minilang_test::runStackVM_IR(src), expected);                                                            \
    EXPECT_EQ(minilang_test::runRegVM(src), expected);

// R150: 验证五后端一致（含 JIT）。
// 当 MINILANG_USE_JIT=ON 时，额外验证 JIT 后端输出等于 expected。
// 当 MINILANG_USE_JIT=OFF 时，退化为四后端（与 EXPECT_ALL_BACKENDS 一致）。
// JIT 后端可能因不支持某些 OpCode 而返回 jit-compile 错误，使用此宏的测试用例
// 必须确保所用语法在 JIT 后端已支持（参见 tests/TestJIT.cpp 的 JIT 支持范围）。
#ifdef MINILANG_USE_JIT
namespace minilang_test {
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
} // namespace minilang_test

#define EXPECT_FIVE_BACKENDS(src, expected)                                                                            \
    EXPECT_EQ(minilang_test::runInterpreter(src), expected);                                                           \
    EXPECT_EQ(minilang_test::runStackVM(src), expected);                                                               \
    EXPECT_EQ(minilang_test::runStackVM_IR(src), expected);                                                            \
    EXPECT_EQ(minilang_test::runRegVM(src), expected);                                                                 \
    EXPECT_EQ(minilang_test::runJIT(src), expected);
#else
#define EXPECT_FIVE_BACKENDS(src, expected) EXPECT_ALL_BACKENDS(src, expected)
#endif
