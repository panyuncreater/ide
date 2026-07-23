// ============================================================
// TestNestedLvalueProbe.cpp — 嵌套左值赋值探针（三后端）
// ------------------------------------------------------------
// 验证嵌套左值表达式（a.b.c / a[i].f / a.b[i] / a[i][j] 等）
// 在 Interpreter / StackVM(IR) / RegisterVM(IR) 三后端上的赋值
// 行为是否一致。含两层基线对照（单层成员 / 单层下标赋值）。
// 每个用例对三种后端分别执行并断言输出完全相同。
// ============================================================

#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>
#include <string>

// 后端运行辅助1：解释器执行，捕获 RuntimeError 等异常，错误时附 <runtime:...> 标记。
static std::string runInterp(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
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
        return "<runtime:" + vm.getLastError() + ">";
    return out;
}
// 后端运行辅助3：寄存器 VM（IR 模式）编译并执行，用于与另两后端交叉验证嵌套左值写回一致性。
static std::string runRegVM_IR(const std::string& src) {
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
        return "<runtime:" + vm.getLastError() + ">";
    return out;
}
// 后端运行辅助4：栈式 VM（非 IR 模式）编译并执行。
// R156 fix: 非 IR 路径（Compiler.cpp direct bytecode）与 IR 路径（IR.cpp）在嵌套左值赋值上
// 应产生相同结果。此 helper 用于验证 R156 修复后两条路径行为一致。
static std::string runStackVM_NonIR(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c; // 默认非 IR 模式
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return "<runtime:" + vm.getLastError() + ">";
    return out;
}

// a.b.c = x (字典)
TEST(NestedLvalueProbe, DictMemberMemberAssign) {
    std::string src = "var d = {\"b\": {\"c\": 0}};\n"
                      "d.b.c = 42;\n"
                      "print(d.b.c);\n";
    EXPECT_EQ(runInterp(src), "42");
    EXPECT_EQ(runStackVM_IR(src), "42");
    EXPECT_EQ(runRegVM_IR(src), "42");
    EXPECT_EQ(runStackVM_NonIR(src), "42"); // R156: 非 IR 路径一致性
}

// a[i].f = x (数组下标 + 字段)
TEST(NestedLvalueProbe, ArrayIndexMemberAssign) {
    std::string src = "var a = [{\"f\": 0}, {\"f\": 0}];\n"
                      "a[0].f = 11;\n"
                      "a[1].f = 22;\n"
                      "print(a[0].f);\n"
                      "print(a[1].f);\n";
    EXPECT_EQ(runInterp(src), "1122");
    EXPECT_EQ(runStackVM_IR(src), "1122");
    EXPECT_EQ(runRegVM_IR(src), "1122");
    EXPECT_EQ(runStackVM_NonIR(src), "1122"); // R156: 非 IR 路径一致性
}

// a.b[i] = x (字段 + 数组下标)
TEST(NestedLvalueProbe, DictMemberIndexAssign) {
    std::string src = "var d = {\"b\": [1, 2, 3]};\n"
                      "d.b[0] = 99;\n"
                      "print(d.b[0]);\n";
    EXPECT_EQ(runInterp(src), "99");
    EXPECT_EQ(runStackVM_IR(src), "99");
    EXPECT_EQ(runRegVM_IR(src), "99");
    EXPECT_EQ(runStackVM_NonIR(src), "99"); // R156: 非 IR 路径一致性
}

// a[i][j] = x (双层数组下标)
TEST(NestedLvalueProbe, ArrayIndexIndexAssign) {
    std::string src = "var a = [[1, 2], [3, 4]];\n"
                      "a[0][1] = 99;\n"
                      "print(a[0][1]);\n";
    EXPECT_EQ(runInterp(src), "99");
    EXPECT_EQ(runStackVM_IR(src), "99");
    EXPECT_EQ(runRegVM_IR(src), "99");
    EXPECT_EQ(runStackVM_NonIR(src), "99"); // R156: 非 IR 路径一致性
}

// 1-level global member assign (基线对照)
TEST(NestedLvalueProbe, SimpleGlobalMemberAssign) {
    std::string src = "var d = {\"x\": 1};\n"
                      "d.x = 42;\n"
                      "print(d.x);\n";
    EXPECT_EQ(runInterp(src), "42");
    EXPECT_EQ(runStackVM_IR(src), "42");
    EXPECT_EQ(runRegVM_IR(src), "42");
}

// 1-level global index assign (基线对照)
TEST(NestedLvalueProbe, SimpleGlobalIndexAssign) {
    std::string src = "var a = [1, 2, 3];\n"
                      "a[0] = 99;\n"
                      "print(a[0]);\n";
    EXPECT_EQ(runInterp(src), "99");
    EXPECT_EQ(runStackVM_IR(src), "99");
    EXPECT_EQ(runRegVM_IR(src), "99");
}

// R156: 循环内嵌套赋值栈平衡验证（四后端一致）
// 修复前非 IR 路径在循环内会栈泄漏（visitMemberAssign 的 compileNode(baseVar)），
// 导致循环数次后栈溢出。R156 修复后改用 LOAD_MUTATED + INDEX_SET_LOCAL/VAR
// 原地修改路径，四后端均正确输出。
// R157 修复：循环次数从 10 提升回 500（原 R156 收尾阶段降级为 10 次规避崩溃）。
// 根因：GcManager 单例悬垂 gcTriggerCallback_（Interpreter 析构未清除）导致
// 后续后端 COW detach 触发 checkIncrementalGc 调用悬垂 this → UAF → bad_alloc。
// 修复：Interpreter 析构清除回调 + GcManager::reset 清除回调（双重保护）。
TEST(NestedLvalueProbe, R156NestedAssignLoopStress) {
    std::string src = "var grid = [[0, 0], [0, 0]];\n"
                      "var d = {\"b\": {\"c\": 0}};\n"
                      "var a = [{\"f\": 0}, {\"f\": 0}];\n"
                      "var i = 0;\n"
                      "while (i < 500) {\n"
                      "  grid[0][0] = i;\n"
                      "  grid[1][1] = i + 1;\n"
                      "  d.b.c = i;\n"
                      "  a[0].f = i;\n"
                      "  i = i + 1;\n"
                      "}\n"
                      "print(grid[0][0]);\n"
                      "print(grid[1][1]);\n"
                      "print(d.b.c);\n"
                      "print(a[0].f);\n";
    // i 最后次 = 499: grid[0][0]=499, grid[1][1]=500, d.b.c=499, a[0].f=499
    std::string expected = "499500499499";
    EXPECT_EQ(runInterp(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM_IR(src), expected);
    EXPECT_EQ(runStackVM_NonIR(src), expected);
}
