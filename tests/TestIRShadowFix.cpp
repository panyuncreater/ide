// TestIRShadowFix.cpp — IR 层「块遮蔽 / 闭包 upvalue」修复回归测试
// 覆盖模块: 解释器、栈式 VM(IR)、寄存器 VM(IR) 三后端在块遮蔽与闭包捕获场景下的行为一致性。
// 验证目标: 针对 CRITICAL-1/2/3 几类历史缺陷（函数内块遮蔽、嵌套顶层块遮蔽、三层闭包 upvalue 透传与修改），
//           同一源码在三后端均须得到相同的打印输出，确保 IR 编译未破坏作用域语义。
// 快速验证 CRITICAL-2/3 块遮蔽修复
#include <gtest/gtest.h>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"  // AUDIT-HELPER fix
#include <string>

// 后端运行辅助1：解释器执行——捕获 RuntimeError/其他异常，返回累积输出（错误时附 <runtime:...> 标记）。
static std::string runInterp(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    // AUDIT-HELPER fix: 对齐 TestConsistencyDiff.cpp 的 runInterp，catch RuntimeError
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}
// 后端运行辅助2：栈式 VM（IR 模式）编译并执行，编译/运行错误分别附 <compile:...>/<runtime:...>。
static std::string runStackVM_IR(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) return "<runtime:" + vm.getLastError() + ">";
    return out;
}
// 后端运行辅助3：寄存器 VM（IR 模式）编译并执行，口径与栈式 VM 一致，用于三方交叉验证。
static std::string runRegVM_IR(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError()) return "<runtime:" + vm.getLastError() + ">";
    return out;
}

// CRITICAL-2: 函数内块遮蔽
TEST(IRShadowFix, InFunctionBlockShadowing) {
    std::string src = "fun f() { var x = 1; { var x = 2; print(x); } print(x); } f();";
    EXPECT_EQ(runInterp(src), "21");
    EXPECT_EQ(runStackVM_IR(src), "21");
    EXPECT_EQ(runRegVM_IR(src), "21");
}

// CRITICAL-3: 嵌套顶层块遮蔽
TEST(IRShadowFix, NestedTopLevelBlockShadowing) {
    std::string src = "var x = 1; { var x = 2; { var x = 3; print(x); } print(x); } print(x);";
    EXPECT_EQ(runInterp(src), "321");
    EXPECT_EQ(runStackVM_IR(src), "321");
    EXPECT_EQ(runRegVM_IR(src), "321");
}

// 三层嵌套函数内遮蔽
TEST(IRShadowFix, TripleNestedInFunctionShadowing) {
    std::string src =
        "fun f() {"
        "  var x = 1;"
        "  { var x = 2; { var x = 3; print(x); } print(x); }"
        "  print(x);"
        "}"
        "f();";
    // 最内 print=3，中间块退出后 print=2，最外层 print=1 → "321"
    EXPECT_EQ(runInterp(src), "321");
    EXPECT_EQ(runStackVM_IR(src), "321");
    EXPECT_EQ(runRegVM_IR(src), "321");
}

// CRITICAL-1: 3层闭包嵌套 upvalue 透传
TEST(IRShadowFix, Closure3LevelNestingUpvalue) {
    std::string src =
        "fun outer() {"
        "  var x = 1;"
        "  fun mid() {"
        "    fun inner() { print(x); }"
        "    inner();"
        "  }"
        "  mid();"
        "}"
        "outer();";
    EXPECT_EQ(runInterp(src), "1");
    EXPECT_EQ(runStackVM_IR(src), "1");
    EXPECT_EQ(runRegVM_IR(src), "1");
}

// CRITICAL-1: 3层闭包嵌套 + 修改 upvalue
TEST(IRShadowFix, Closure3LevelNestingUpvalueModify) {
    std::string src =
        "fun outer() {"
        "  var x = 1;"
        "  fun mid() {"
        "    fun inner() { x = x + 10; }"
        "    inner();"
        "  }"
        "  mid();"
        "  print(x);"
        "}"
        "outer();";
    EXPECT_EQ(runInterp(src), "11");
    EXPECT_EQ(runStackVM_IR(src), "11");
    EXPECT_EQ(runRegVM_IR(src), "11");
}
