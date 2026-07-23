// 临时边界场景审计 - 验证 RegisterVM 闭包调用潜在 bug
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

static std::string runInterpreter(const std::string& src) {
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
static std::string runStackVM(const std::string& src) {
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
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}
static std::string runRegVM(const std::string& src) {
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

// 验证四后端一致
#define EXPECT_ALL_BACKENDS(src, expected)                                                                             \
    EXPECT_EQ(runInterpreter(src), expected);                                                                          \
    EXPECT_EQ(runStackVM(src), expected);                                                                              \
    EXPECT_EQ(runStackVM_IR(src), expected);                                                                           \
    EXPECT_EQ(runRegVM(src), expected);

// S1: 闭包修改外层局部变量（open upvalue 写回）
TEST(RegisterVMClosureAudit, S1_ClosureModifiesOuterLocal) {
    std::string src = "fun outer() {"
                      "  var x = 10;"
                      "  fun inc() { x = x + 1; return x; }"
                      "  var a = inc();"
                      "  var b = inc();"
                      "  return x + a + b;"
                      "}"
                      "print(outer());";
    EXPECT_ALL_BACKENDS(src, "35"); // x初=10; inc()→x=11,return 11=a; inc()→x=12,return 12=b; x+a+b=12+11+12=35
}

// S2: 多闭包共享同一 upvalue
TEST(RegisterVMClosureAudit, S2_MultipleClosuresShareUpvalue) {
    std::string src = "fun outer() {"
                      "  var x = 0;"
                      "  fun inc() { x = x + 1; }"
                      "  fun get() { return x; }"
                      "  inc();"
                      "  inc();"
                      "  return get();"
                      "}"
                      "print(outer());";
    EXPECT_ALL_BACKENDS(src, "2");
}

// S3: 闭包递归调用自身（通过 var 自引用）
TEST(RegisterVMClosureAudit, S3_ClosureRecursionViaVar) {
    std::string src = "fun outer() {"
                      "  var fib = null;"
                      "  fib = fun(n) {"
                      "    if (n < 2) return n;"
                      "    return fib(n - 1) + fib(n - 2);"
                      "  };"
                      "  return fib(10);"
                      "}"
                      "print(outer());";
    EXPECT_ALL_BACKENDS(src, "55");
}

// S4: for 循环中创建闭包（var 语义：所有闭包共享同一变量）
TEST(RegisterVMClosureAudit, S4_ClosureInForLoop) {
    std::string src = "fun main() {"
                      "  var fns = [];"
                      "  for (var i = 0; i < 3; i = i + 1) {"
                      "    fns.push(fun() { return i; });"
                      "  }"
                      "  return fns[0]() + fns[1]() + fns[2]();"
                      "}"
                      "print(main());";
    // MiniLang var 语义：i 是 outer 变量，循环结束后 i=3，所有闭包返回 3
    EXPECT_ALL_BACKENDS(src, "9");
}

// S5: 闭包作为字典值 + 立即调用
TEST(RegisterVMClosureAudit, S5_ClosureAsDictValueCall) {
    std::string src = "fun main() {"
                      "  fun double(x) { return x * 2; }"
                      "  var d = {\"fn\": double};"
                      "  return d[\"fn\"](21);"
                      "}"
                      "print(main());";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S6: 闭包作为元组元素 + 立即调用
TEST(RegisterVMClosureAudit, S6_ClosureAsTupleElementCall) {
    std::string src = "fun main() {"
                      "  fun inc(x) { return x + 1; }"
                      "  var t = (inc, 100);"
                      "  return t[0](41);"
                      "}"
                      "print(main());";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S7: 闭包作为函数参数
TEST(RegisterVMClosureAudit, S7_ClosureAsFunctionArg) {
    std::string src = "fun apply(f, x) { return f(x); }"
                      "fun double(x) { return x * 2; }"
                      "print(apply(double, 21));";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S8: 闭包作为高阶函数参数（map 内调用闭包）
TEST(RegisterVMClosureAudit, S8_ClosureAsHigherOrderArg) {
    std::string src = "fun double(x) { return x * 2; }"
                      "var arr = [1, 2, 3];"
                      "var result = map(arr, double);"
                      "print(result[0] + result[1] + result[2]);";
    EXPECT_ALL_BACKENDS(src, "12");
}

// S9: 闭包在 try/catch 中创建并捕获 try 块局部变量
TEST(RegisterVMClosureAudit, S9_ClosureInTryBlock) {
    std::string src = "fun main() {"
                      "  var f = null;"
                      "  try {"
                      "    var x = 42;"
                      "    f = fun() { return x; };"
                      "  } catch (e) {"
                      "    return -1;"
                      "  }"
                      "  return f();"
                      "}"
                      "print(main());";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S10: 闭包在 catch 块中创建并捕获 catch 变量
TEST(RegisterVMClosureAudit, S10_ClosureInCatchBlock) {
    std::string src = "fun main() {"
                      "  var f = null;"
                      "  try {"
                      "    throw \"err\";"
                      "  } catch (e) {"
                      "    f = fun() { return e; };"
                      "  }"
                      "  return f();"
                      "}"
                      "print(main());";
    EXPECT_ALL_BACKENDS(src, "err");
}

// S11: 闭包作为类字段 + 方法中调用（先取字段再调用）
TEST(RegisterVMClosureAudit, S11_ClosureAsInstanceField) {
    std::string src = "class Calc {"
                      "  var fn;"
                      "  fun init(f) { this.fn = f; }"
                      "  fun call(x) { var f = this.fn; return f(x); }"
                      "}"
                      "fun double(x) { return x * 2; }"
                      "var c = Calc(double);"
                      "print(c.call(21));";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S12: 闭包默认参数 + 闭包变量调用
TEST(RegisterVMClosureAudit, S12_ClosureWithDefaultArg) {
    std::string src = "fun main() {"
                      "  fun add(a, b = 10) { return a + b; }"
                      "  var g = add;"
                      "  return g(5) + g(5, 20);"
                      "}"
                      "print(main());";
    EXPECT_ALL_BACKENDS(src, "40"); // (5+10) + (5+20) = 15+25 = 40
}

// S13: 3 层嵌套 lambda + 透传 upvalue
TEST(RegisterVMClosureAudit, S13_ThreeLayerNestedLambda) {
    std::string src = "fun outer() {"
                      "  var x = 1;"
                      "  fun mid() {"
                      "    fun inner() {"
                      "      fun core() { return x + 41; }"
                      "      return core();"
                      "    }"
                      "    return inner();"
                      "  }"
                      "  return mid();"
                      "}"
                      "print(outer());";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S14: 闭包作为数组元素 + 通过索引调用（已有 InvokeEach 测试，这里单独验证）
TEST(RegisterVMClosureAudit, S14_ClosureArrayIndexCall) {
    std::string src = "fun main() {"
                      "  fun double(x) { return x * 2; }"
                      "  var arr = [double];"
                      "  return arr[0](21);"
                      "}"
                      "print(main());";
    EXPECT_ALL_BACKENDS(src, "42");
}

// S15: 闭包内 try/throw 异常展开 + upvalue 关闭
TEST(RegisterVMClosureAudit, S15_ClosureThrowException) {
    std::string src = "fun main() {"
                      "  var x = 10;"
                      "  fun f() {"
                      "    x = x + 5;"
                      "    throw \"err\";"
                      "    return x;" // 不可达
                      "  }"
                      "  try {"
                      "    f();"
                      "  } catch (e) {"
                      "    return x;"
                      "  }"
                      "}"
                      "print(main());";
    EXPECT_ALL_BACKENDS(src, "15"); // 闭包内修改 x=15，throw 后 catch 读取 x=15
}
