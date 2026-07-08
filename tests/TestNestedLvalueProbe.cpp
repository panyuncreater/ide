// ============================================================
// TestNestedLvalueProbe.cpp — 嵌套左值赋值探针（三后端）
// ------------------------------------------------------------
// 验证嵌套左值表达式（a.b.c / a[i].f / a.b[i] / a[i][j] 等）
// 在 Interpreter / StackVM(IR) / RegisterVM(IR) 三后端上的赋值
// 行为是否一致。含两层基线对照（单层成员 / 单层下标赋值）。
// 每个用例对三种后端分别执行并断言输出完全相同。
// ============================================================

#include <gtest/gtest.h>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Interpreter.h"
#include <string>

// 后端运行辅助1：解释器执行，捕获 RuntimeError 等异常，错误时附 <runtime:...> 标记。
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
// 后端运行辅助3：寄存器 VM（IR 模式）编译并执行，用于与另两后端交叉验证嵌套左值写回一致性。
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

// a.b.c = x (字典)
TEST(NestedLvalueProbe, DictMemberMemberAssign) {
    std::string src =
        "var d = {\"b\": {\"c\": 0}};\n"
        "d.b.c = 42;\n"
        "print(d.b.c);\n";
    EXPECT_EQ(runInterp(src), "42");
    EXPECT_EQ(runStackVM_IR(src), "42");
    EXPECT_EQ(runRegVM_IR(src), "42");
}

// a[i].f = x (数组下标 + 字段)
TEST(NestedLvalueProbe, ArrayIndexMemberAssign) {
    std::string src =
        "var a = [{\"f\": 0}, {\"f\": 0}];\n"
        "a[0].f = 11;\n"
        "a[1].f = 22;\n"
        "print(a[0].f);\n"
        "print(a[1].f);\n";
    EXPECT_EQ(runInterp(src), "1122");
    EXPECT_EQ(runStackVM_IR(src), "1122");
    EXPECT_EQ(runRegVM_IR(src), "1122");
}

// a.b[i] = x (字段 + 数组下标)
TEST(NestedLvalueProbe, DictMemberIndexAssign) {
    std::string src =
        "var d = {\"b\": [1, 2, 3]};\n"
        "d.b[0] = 99;\n"
        "print(d.b[0]);\n";
    EXPECT_EQ(runInterp(src), "99");
    EXPECT_EQ(runStackVM_IR(src), "99");
    EXPECT_EQ(runRegVM_IR(src), "99");
}

// a[i][j] = x (双层数组下标)
TEST(NestedLvalueProbe, ArrayIndexIndexAssign) {
    std::string src =
        "var a = [[1, 2], [3, 4]];\n"
        "a[0][1] = 99;\n"
        "print(a[0][1]);\n";
    EXPECT_EQ(runInterp(src), "99");
    EXPECT_EQ(runStackVM_IR(src), "99");
    EXPECT_EQ(runRegVM_IR(src), "99");
}

// 1-level global member assign (基线对照)
TEST(NestedLvalueProbe, SimpleGlobalMemberAssign) {
    std::string src =
        "var d = {\"x\": 1};\n"
        "d.x = 42;\n"
        "print(d.x);\n";
    EXPECT_EQ(runInterp(src), "42");
    EXPECT_EQ(runStackVM_IR(src), "42");
    EXPECT_EQ(runRegVM_IR(src), "42");
}

// 1-level global index assign (基线对照)
TEST(NestedLvalueProbe, SimpleGlobalIndexAssign) {
    std::string src =
        "var a = [1, 2, 3];\n"
        "a[0] = 99;\n"
        "print(a[0]);\n";
    EXPECT_EQ(runInterp(src), "99");
    EXPECT_EQ(runStackVM_IR(src), "99");
    EXPECT_EQ(runRegVM_IR(src), "99");
}
