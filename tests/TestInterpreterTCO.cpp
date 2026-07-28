// ============================================================
// TestInterpreterTCO — B1: Interpreter 尾调用蹦床（trampoline）回归锁
// ------------------------------------------------------------
// 背景（testing.md 原已知限制 #10）：深尾递归（return f(n-1) 形态）在
// StackVM/StackVM-IR/RegisterVM 经 TCO 帧复用恒定栈深完成，而 Interpreter
// 无帧复用机制，超过 256 层报"递归深度超过限制"——三后端最后一个已知
// 设计差异。B1 在 visitReturnStmt 引入 TailCallSignal + callNamedFunction/
// invokeMethod 蹦床循环消除该差异。
//
// 覆盖场景：
//   1. SelfFunction 深尾递归（10 万层）四后端一致
//   2. SelfMethod 深尾递归（this.m(n-1)）四后端一致
//   3. 语义保持：非尾递归仍受深度限制；try 内 return 不 TCO（finally 语义）；
//      名称重绑定遮蔽时回退普通调用；子类 override 保持虚分派
//   4. 默认参数 + 闭包捕获与 TCO 交互
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <string>

namespace {

std::string runInterpreter(const std::string& src) {
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

std::string runStackVM(const std::string& src) {
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

std::string runStackVM_IR(const std::string& src) {
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

std::string runRegVM(const std::string& src) {
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

void expectAllBackends(const std::string& src, const std::string& expected) {
    EXPECT_EQ(runInterpreter(src), expected) << "Interpreter 路径";
    EXPECT_EQ(runStackVM(src), expected) << "StackVM 路径";
    EXPECT_EQ(runStackVM_IR(src), expected) << "StackVM-IR 路径";
    EXPECT_EQ(runRegVM(src), expected) << "RegisterVM 路径";
}

} // namespace

// ============================================================
// 第一组：SelfFunction 深尾递归四后端一致（核心场景，原 G7/E6 差异）
// ============================================================

// 累加器尾递归：100000 层，Interpreter 原报"递归深度超过限制"，现恒定栈深完成
TEST(InterpreterTCO, DeepTailRecursionAccumulator) {
    const std::string src = "fun f(n, acc) {\n"
                            "    if (n == 0) { return acc; }\n"
                            "    return f(n - 1, acc + n);\n"
                            "}\n"
                            "print(f(100000, 0));";
    expectAllBackends(src, "5000050000");
}

// 无累加器的计数尾递归
TEST(InterpreterTCO, DeepTailRecursionCountdown) {
    const std::string src = "fun down(n) {\n"
                            "    if (n <= 0) { return \"done\"; }\n"
                            "    return down(n - 1);\n"
                            "}\n"
                            "print(down(50000));";
    expectAllBackends(src, "done");
}

// 尾递归 + 默认参数：缺省实参在每轮帧复用时用默认值填充
TEST(InterpreterTCO, TailRecursionWithDefaultParam) {
    const std::string src = "fun f(n, step = 1) {\n"
                            "    if (n <= 0) { return \"ok\"; }\n"
                            "    return f(n - step);\n"
                            "}\n"
                            "print(f(10000));";
    expectAllBackends(src, "ok");
}

// 返回类型注解 + 尾递归：基例返回处检查一次，观测语义不变
TEST(InterpreterTCO, TailRecursionWithReturnType) {
    const std::string src = "int f(int n, int acc) {\n"
                            "    if (n == 0) { return acc; }\n"
                            "    return f(n - 1, acc + 1);\n"
                            "}\n"
                            "print(f(20000, 0));";
    expectAllBackends(src, "20000");
}

// ============================================================
// 第二组：SelfMethod 深尾递归（return this.m(n-1)）
// ============================================================

TEST(InterpreterTCO, DeepMethodTailRecursion) {
    const std::string src = "class C {\n"
                            "    fun count(n, acc) {\n"
                            "        if (n == 0) { return acc; }\n"
                            "        return this.count(n - 1, acc + 1);\n"
                            "    }\n"
                            "}\n"
                            "var c = C();\n"
                            "print(c.count(50000, 0));";
    expectAllBackends(src, "50000");
}

// 方法尾递归期间修改字段：帧复用取回每轮更新后的 this
TEST(InterpreterTCO, MethodTailRecursionMutatesField) {
    const std::string src = "class Acc {\n"
                            "    var total = 0;\n"
                            "    fun add(n) {\n"
                            "        if (n == 0) { return this.total; }\n"
                            "        this.total = this.total + n;\n"
                            "        return this.add(n - 1);\n"
                            "    }\n"
                            "}\n"
                            "var a = Acc();\n"
                            "print(a.add(1000));\n"
                            "print(a.total);";
    expectAllBackends(src, "500500500500");
}

// ============================================================
// 第三组：语义保持（TCO 是纯优化，不改变可观测行为）
// ============================================================

// 非尾递归（return f(n-1) + n）不 TCO：Interpreter 仍报递归深度错误（原有语义）
TEST(InterpreterTCO, NonTailRecursionStillDepthLimited) {
    const std::string src = "fun f(n) {\n"
                            "    if (n == 0) { return 0; }\n"
                            "    return f(n - 1) + n;\n"
                            "}\n"
                            "print(f(100000));";
    std::string out = runInterpreter(src);
    EXPECT_NE(out.find("递归深度"), std::string::npos) << "实际: " << out;
}

// 浅层非尾递归正常工作（回归保护）
TEST(InterpreterTCO, ShallowNonTailRecursionUnaffected) {
    const std::string src = "fun fib(n) {\n"
                            "    if (n < 2) { return n; }\n"
                            "    return fib(n - 1) + fib(n - 2);\n"
                            "}\n"
                            "print(fib(15));";
    expectAllBackends(src, "610");
}

// try 块内的 return f(n-1) 不 TCO（finally 语义保留），浅层结果正确
TEST(InterpreterTCO, TailCallInsideTryNotOptimized) {
    const std::string src = "var log = \"\";\n"
                            "fun f(n) {\n"
                            "    try {\n"
                            "        if (n == 0) { return 0; }\n"
                            "        return f(n - 1);\n"
                            "    } finally {\n"
                            "        log = log + \"F\";\n"
                            "    }\n"
                            "}\n"
                            "f(3);\n"
                            "print(log);";
    // 每层递归退出时都执行 finally：4 层调用 → 4 个 F
    expectAllBackends(src, "FFFF");
}

// 名称重绑定遮蔽：局部 var f 遮蔽函数名时回退普通调用（不误跳自身）
TEST(InterpreterTCO, ShadowedNameFallsBackToNormalCall) {
    const std::string src = "fun helper(n) { return n * 10; }\n"
                            "fun f(n) {\n"
                            "    var f = helper;\n"
                            "    return f(n + 1);\n"
                            "}\n"
                            "print(f(3));";
    expectAllBackends(src, "40");
}

// 子类 override：父类方法内 this.m() 必须虚分派到子类（不帧复用父类方法）
TEST(InterpreterTCO, OverriddenMethodKeepsVirtualDispatch) {
    const std::string src = "class Base {\n"
                            "    fun m(n) {\n"
                            "        if (n == 0) { return \"base\"; }\n"
                            "        return this.m(n - 1);\n"
                            "    }\n"
                            "    fun go(n) { return this.m(n); }\n"
                            "}\n"
                            "class Sub extends Base {\n"
                            "    fun m(n) { return \"sub\"; }\n"
                            "}\n"
                            "var s = Sub();\n"
                            "print(s.go(5));";
    expectAllBackends(src, "sub");
}

// 尾递归函数内定义闭包捕获本轮参数：每轮独立捕获（帧复用不共享环境）
TEST(InterpreterTCO, ClosureCaptureInsideTailRecursion) {
    const std::string src = "var saved = [];\n"
                            "fun f(n) {\n"
                            "    fun snap() { return n; }\n"
                            "    saved.push(snap);\n"
                            "    if (n == 0) { return 0; }\n"
                            "    return f(n - 1);\n"
                            "}\n"
                            "f(2);\n"
                            "print(saved[0]());\n"
                            "print(saved[2]());";
    expectAllBackends(src, "20");
}

// 互递归不 TCO（g != f，跨函数跳转不支持），浅层结果正确
TEST(InterpreterTCO, MutualRecursionUnaffected) {
    const std::string src = "fun isEven(n) {\n"
                            "    if (n == 0) { return true; }\n"
                            "    return isOdd(n - 1);\n"
                            "}\n"
                            "fun isOdd(n) {\n"
                            "    if (n == 0) { return false; }\n"
                            "    return isEven(n - 1);\n"
                            "}\n"
                            "print(isEven(100));";
    expectAllBackends(src, "true");
}
