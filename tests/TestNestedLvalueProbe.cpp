// 临时探针：验证嵌套左值 a.b.c = x 在三后端的行为
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
