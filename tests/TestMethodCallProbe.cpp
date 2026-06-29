// 临时探针：验证 IR 路径下方法调用变异写回是否工作
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

// 全局变量 arr.push(4) — 应输出 [1, 2, 3, 4]
TEST(MethodCallProbe, GlobalArrayPush) {
    std::string src =
        "var arr = [1, 2, 3];\n"
        "arr.push(4);\n"
        "print(arr);\n";
    EXPECT_EQ(runInterp(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runStackVM_IR(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runRegVM_IR(src), "[1, 2, 3, 4]");
}

// 全局变量 arr.pop() — 应输出 3 然后 [1, 2]
TEST(MethodCallProbe, GlobalArrayPop) {
    std::string src =
        "var arr = [1, 2, 3];\n"
        "var x = arr.pop();\n"
        "print(x);\n"
        "print(arr);\n";
    EXPECT_EQ(runInterp(src), "3[1, 2]");
    EXPECT_EQ(runStackVM_IR(src), "3[1, 2]");
    EXPECT_EQ(runRegVM_IR(src), "3[1, 2]");
}

// 局部变量 arr.push(4)（在函数内）
TEST(MethodCallProbe, LocalArrayPush) {
    std::string src =
        "func f() {\n"
        "  var arr = [1, 2, 3];\n"
        "  arr.push(4);\n"
        "  print(arr);\n"
        "}\n"
        "f();\n";
    EXPECT_EQ(runInterp(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runStackVM_IR(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runRegVM_IR(src), "[1, 2, 3, 4]");
}

// 嵌套访问变异方法：this.arr.push(42)
TEST(MethodCallProbe, NestedMemberAccessPush) {
    std::string src =
        "class C {\n"
        "  var arr;\n"
        "  func init() { this.arr = [1, 2, 3]; }\n"
        "  func add() { this.arr.push(4); }\n"
        "}\n"
        "var c = C();\n"
        "c.add();\n"
        "print(c.arr);\n";
    EXPECT_EQ(runInterp(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runStackVM_IR(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runRegVM_IR(src), "[1, 2, 3, 4]");
}

// 嵌套访问变异方法：arr[0].push(42)
TEST(MethodCallProbe, NestedIndexAccessPush) {
    std::string src =
        "var a = [[1, 2], [3, 4]];\n"
        "a[0].push(99);\n"
        "print(a[0]);\n";
    EXPECT_EQ(runInterp(src), "[1, 2, 99]");
    EXPECT_EQ(runStackVM_IR(src), "[1, 2, 99]");
    EXPECT_EQ(runRegVM_IR(src), "[1, 2, 99]");
}

// 字典 set 变异方法
TEST(MethodCallProbe, DictSetMethod) {
    std::string src =
        "var d = {};\n"
        "d.set(\"k\", 42);\n"
        "print(d.get(\"k\"));\n";
    EXPECT_EQ(runInterp(src), "42");
    EXPECT_EQ(runStackVM_IR(src), "42");
    EXPECT_EQ(runRegVM_IR(src), "42");
}
