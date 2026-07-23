// ============================================================
// R103 闭包与高阶函数增强测试套件
// ------------------------------------------------------------
// 验证闭包作为一等公民、currying、upvalue 闭包变量调用在三后端
// (Interpreter / StackVM / StackVM-IR / RegisterVM) 上的语义一致性。
//
// 重点覆盖 W1 fix (R103)：StackVM 直接路径 `var g = f; g();` 修复
// （对齐 IR.cpp CRITICAL-1 fix），新增 OP_GET_LOCAL/OP_GET_UPVALUE +
// OP_CALL_EXPR 路径替代原 OP_CALL 按名查找。
//
// 覆盖点：
//   1. var g = f; g(); 三后端一致性（0/1/2/3 参数）
//   2. 闭包作为字典值
//   3. 闭包作为元组元素
//   4. deep currying f(a)(b)(c)(d)
//   5. upvalue 闭包变量调用（嵌套函数中通过 upvalue 调用闭包）
//   6. if 块内命名函数 + 引用赋值（原 StackVM 限制场景修复验证）
//   7. 闭包作为数组元素 + 索引调用
//   8. 闭包作为函数返回值 + 立即调用
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <string>

// ============================================================
// 辅助：四后端执行器
// ============================================================

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
        if (out.empty())
            return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
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
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
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
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
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
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

// ============================================================
// W1 fix (R103) 验证：var g = f; g(); 三后端一致性
// ============================================================

TEST(ClosureVariableCall, ZeroArg) {
    // 嵌套函数 + var g = f; g(); 场景（避免依赖全局函数名作为表达式引用的独立预存在问题）
    std::string src = "fun main() {"
                      "  fun f() { return 42; }"
                      "  var g = f;"
                      "  return g();"
                      "}"
                      "print(main());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(ClosureVariableCall, OneArg) {
    std::string src = "fun main() {"
                      "  fun inc(x) { return x + 1; }"
                      "  var g = inc;"
                      "  return g(41);"
                      "}"
                      "print(main());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(ClosureVariableCall, TwoArgs) {
    std::string src = "fun main() {"
                      "  fun add(a, b) { return a + b; }"
                      "  var g = add;"
                      "  return g(20, 22);"
                      "}"
                      "print(main());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(ClosureVariableCall, ThreeArgs) {
    std::string src = "fun main() {"
                      "  fun add3(a, b, c) { return a + b + c; }"
                      "  var g = add3;"
                      "  return g(10, 20, 12);"
                      "}"
                      "print(main());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(ClosureVariableCall, ReassignMultipleTimes) {
    std::string src = "fun main() {"
                      "  fun add(a, b) { return a + b; }"
                      "  fun mul(a, b) { return a * b; }"
                      "  var g = add;"
                      "  var s1 = g(20, 22);"
                      "  g = mul;"
                      "  var s2 = g(6, 7);"
                      "  return s1 + s2;"
                      "}"
                      "print(main());";
    std::string expected = "84"; // 42 + 42
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// W1 fix 扩展验证：顶层 var 持闭包值调用
// （var g = makeAdder(10); g(32); makeAdder 是顶层 fun，返回闭包，g 注册到 globalSlot_）
// 这是 W1 fix 扩展路径（inFunction_==false + lookupGlobalSlot 命中）
// ============================================================

TEST(ClosureVariableCall, TopLevelVarHoldsClosure) {
    // makeAdder 是顶层 fun 声明，g 是顶层 var 持闭包值
    // W1 fix 扩展：visitFunCall 检测到 g 在 globalSlot_ 中，走 OP_GET_GLOBAL + OP_CALL_EXPR
    std::string src = "fun makeAdder(n) {"
                      "  fun add(x) { return x + n; }"
                      "  return add;"
                      "}"
                      "var g = makeAdder(32);"
                      "print(g(10));";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 闭包作为字典值
// ============================================================

TEST(ClosureAsValue, AsDictValue) {
    // 嵌套函数 + var 持闭包值，再存入字典（避免依赖"全局函数名作为表达式引用"
    // 这一独立预存在问题——顶层 FunDecl 不注册 globalSlot_，本测试通过嵌套函数规避）
    std::string src = "fun main() {"
                      "  fun double(x) { return x * 2; }"
                      "  fun triple(x) { return x * 3; }"
                      "  var ops = { \"double\": double, \"triple\": triple };"
                      "  return ops[\"double\"](21) + ops[\"triple\"](14);"
                      "}"
                      "print(main());";
    std::string expected = "84"; // 42 + 42
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 闭包作为元组元素
// ============================================================

TEST(ClosureAsValue, AsTupleElement) {
    std::string src = "fun main() {"
                      "  fun inc(x) { return x + 1; }"
                      "  fun dec(x) { return x - 1; }"
                      "  var t = (inc, dec);"
                      "  return t[0](41) + t[1](43);"
                      "}"
                      "print(main());";
    std::string expected = "84"; // 42 + 42
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 闭包作为数组元素 + 索引调用
// ============================================================

TEST(ClosureAsValue, AsArrayElement) {
    std::string src = "fun main() {"
                      "  fun double(x) { return x * 2; }"
                      "  fun triple(x) { return x * 3; }"
                      "  var fns = [double, triple];"
                      "  var h = fns[0];"
                      "  return fns[0](21) + fns[1](14) + h(50);"
                      "}"
                      "print(main());";
    std::string expected = "184"; // 42 + 42 + 100
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// deep currying: f(a)(b)(c)(d)
// ============================================================

TEST(Currying, DeepCurryingThreeLevels) {
    std::string src = "fun add(a) {"
                      "  fun inner1(b) {"
                      "    fun inner2(c) {"
                      "      return a + b + c;"
                      "    }"
                      "    return inner2;"
                      "  }"
                      "  return inner1;"
                      "}"
                      "print(add(10)(20)(12));";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(Currying, DeepCurryingFourLevels) {
    std::string src = "fun f(a) {"
                      "  fun g(b) {"
                      "    fun h(c) {"
                      "      fun i(d) {"
                      "        return a + b + c + d;"
                      "      }"
                      "      return i;"
                      "    }"
                      "    return h;"
                      "  }"
                      "  return g;"
                      "}"
                      "print(f(10)(15)(10)(7));";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(Currying, CurryingWithClosureCapture) {
    // 每层 currying 都捕获外层变量，验证 upvalue 透传链
    std::string src = "fun makeAdder(x) {"
                      "  fun add(y) { return x + y; }"
                      "  return add;"
                      "}"
                      "var add10 = makeAdder(10);"
                      "var add20 = makeAdder(20);"
                      "print(add10(32));"
                      "print(add20(22));";
    std::string expected = "4242"; // MiniLang print 不自动加换行
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// upvalue 闭包变量调用：嵌套函数中通过 upvalue 调用闭包
// ============================================================

TEST(ClosureVariableCall, ViaUpvalue) {
    // 外层函数声明 g 持有闭包值；内层函数通过 upvalue 调用 g
    std::string src = "fun outer() {"
                      "  fun f(x) { return x * 2; }"
                      "  var g = f;"
                      "  fun inner() { return g(21); }" // inner 通过 upvalue 引用 g
                      "  return inner();"
                      "}"
                      "print(outer());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(ClosureVariableCall, ViaUpvalueDeep) {
    // upvalue 透传 2 层后调用
    std::string src = "fun outer() {"
                      "  fun f(x) { return x + 1; }"
                      "  var g = f;"
                      "  fun middle() {"
                      "    fun inner() { return g(41); }" // inner 通过 middle 透传到 outer 的 g
                      "    return inner();"
                      "  }"
                      "  return middle();"
                      "}"
                      "print(outer());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// if 块内命名函数 + 引用赋值（原 StackVM 限制场景修复验证）
// ============================================================

TEST(ClosureVariableCall, IfBlockNamedFuncReassign) {
    // 对齐 TestAuditBatch1.IfBlockClosureSnapshot 场景
    // 原 StackVM 直接路径报 "未定义的函数: g"，W1 fix 后应返回 42
    std::string src = "fun makeF() {"
                      "  var x = 42;"
                      "  var g = null;"
                      "  if (true) {"
                      "    fun f() { return x; }"
                      "    g = f;"
                      "  }"
                      "  if (true) { var dummy = 0; }"
                      "  return g();"
                      "}"
                      "print(makeF());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(ClosureVariableCall, WhileBlockNamedFuncReassign) {
    // while 块内命名函数 + 引用赋值（变体）
    std::string src = "fun makeF() {"
                      "  var x = 42;"
                      "  var g = null;"
                      "  var i = 0;"
                      "  while (i < 1) {"
                      "    fun f() { return x; }"
                      "    g = f;"
                      "    i = i + 1;"
                      "  }"
                      "  return g();"
                      "}"
                      "print(makeF());";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 闭包作为函数返回值 + 立即调用
// ============================================================

TEST(ClosureReturnValue, ImmediatelyInvoked) {
    // makeAdder(10) 返回闭包，立即调用
    std::string src = "fun makeAdder(n) {"
                      "  fun add(x) { return x + n; }"
                      "  return add;"
                      "}"
                      "print(makeAdder(32)(10));";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(ClosureReturnValue, ReturnedClosureCapturesArg) {
    // 返回的闭包捕获外层参数
    std::string src = "fun multiplier(factor) {"
                      "  fun apply(x) { return x * factor; }"
                      "  return apply;"
                      "}"
                      "var double = multiplier(2);"
                      "var triple = multiplier(3);"
                      "print(double(21));"
                      "print(triple(14));";
    std::string expected = "4242"; // MiniLang print 不自动加换行
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 闭包数组遍历调用
// ============================================================

TEST(ClosureArrayTraversal, InvokeEach) {
    // 数组中存多个闭包，for 循环遍历调用（嵌套函数场景）
    std::string src = "fun main() {"
                      "  fun double(x) { return x * 2; }"
                      "  fun triple(x) { return x * 3; }"
                      "  fun quad(x) { return x * 4; }"
                      "  var fns = [double, triple, quad];"
                      "  var sum = 0;"
                      "  for (var i = 0; i < 3; i = i + 1) {"
                      "    sum = sum + fns[i](10);"
                      "  }"
                      "  return sum;"
                      "}"
                      "print(main());";
    std::string expected = "90"; // 20 + 30 + 40
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 边界：闭包变量未赋值调用（null 调用）
// ============================================================

TEST(ClosureVariableCallBoundary, UnassignedNullCall) {
    // var g = null; g(); 应报运行时错误（不是函数），三后端一致
    std::string src = "var g = null;"
                      "g();";
    // 三后端均应报 runtime error，错误消息可能略有差异，统一验证包含 "runtime"
    EXPECT_NE(runInterpreter(src).find("<runtime:"), std::string::npos);
    EXPECT_NE(runStackVM(src).find("<runtime:"), std::string::npos);
    EXPECT_NE(runStackVM_IR(src).find("<runtime:"), std::string::npos);
    EXPECT_NE(runRegVM(src).find("<runtime:"), std::string::npos);
}

// ============================================================
// catch 变量被闭包捕获（对齐 TestAuditBatch4，补 StackVM 直接路径）
// ============================================================

TEST(ClosureVariableCall, CatchVarCapturedAndInvoked_StackVM) {
    // R103 W1 fix 后 StackVM 直接路径也支持 closure() 局部闭包变量调用，
    // 此前仅 Interpreter / IR / RegVM 路径覆盖。本测试补 StackVM 直接路径。
    std::string src = R"(
fun f() {
    var closure;
    try {
        throw 42;
    } catch (e) {
        fun getter() { return e; }
        closure = getter;
    }
    var x = 99;
    return closure();
}
print(f());
)";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}
