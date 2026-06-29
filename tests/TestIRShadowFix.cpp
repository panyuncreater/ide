// 快速验证 CRITICAL-2/3 块遮蔽修复
#include <gtest/gtest.h>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Interpreter.h"
#include <string>

static std::string runInterp(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    interp.execute(*ast);
    return out;
}
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
