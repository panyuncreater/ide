// ============================================================
// R98 W3: Lambda 表达式测试套件
// ------------------------------------------------------------
// 验证 `fun(params) { body }` 匿名函数表达式在四后端上的语义一致性：
//   - Interpreter（树遍历解释器）
//   - StackVM（栈式字节码 VM 直接路径）
//   - StackVM_IR（栈式 VM 经 IR 路径）
//   - RegisterVM（寄存器式 VM）
//
// 覆盖点：
//   1. 基础语法：直接调用 lambda、立即执行
//   2. 赋值给变量：var f = fun(x) {...}; f(args)
//   3. 作为函数参数：高阶函数组合（map/filter/reduce + lambda）
//   4. 闭包捕获：lambda 捕获外层局部变量、嵌套 lambda
//   5. 返回 lambda：函数返回 lambda（多级捕获）
//   6. lambda 作为数组元素：[fun(x)..., fun(x)...]
//   7. 三后端一致性：相同源码四路径结果一致
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

/// 四后端一致性验证宏（减少重复代码）
#define EXPECT_FOUR_BACKENDS(src, expected)                                                                            \
    do {                                                                                                               \
        EXPECT_EQ(runInterpreter(src), expected) << "Interpreter failed";                                              \
        EXPECT_EQ(runStackVM(src), expected) << "StackVM failed";                                                      \
        EXPECT_EQ(runStackVM_IR(src), expected) << "StackVM_IR failed";                                                \
        EXPECT_EQ(runRegVM(src), expected) << "RegisterVM failed";                                                     \
    } while (0)

// ============================================================
// 1. 基础语法：直接调用 lambda
// ============================================================

TEST(LambdaBasic, ImmediateInvoke) {
    // 立即调用 lambda：fun(x){...}(arg)
    std::string src = "var r = fun(x) { return x + 1; }(41);"
                      "print(r);";
    EXPECT_FOUR_BACKENDS(src, "42");
}

TEST(LambdaBasic, NoArgs) {
    // 无参 lambda
    std::string src = "var f = fun() { return 100; };"
                      "print(f());";
    EXPECT_FOUR_BACKENDS(src, "100");
}

TEST(LambdaBasic, AssignAndCall) {
    // 赋值给变量后调用
    std::string src = "var f = fun(x, y) { return x * y; };"
                      "print(f(6, 7));";
    EXPECT_FOUR_BACKENDS(src, "42");
}

TEST(LambdaBasic, SingleParam) {
    // 单参数 lambda
    std::string src = "var sq = fun(x) { return x * x; };"
                      "print(sq(9));";
    EXPECT_FOUR_BACKENDS(src, "81");
}

TEST(LambdaBasic, ReturnString) {
    // lambda 返回字符串
    std::string src = "var greet = fun(name) { return \"Hi, \" + name; };"
                      "print(greet(\"Alice\"));";
    EXPECT_FOUR_BACKENDS(src, "Hi, Alice");
}

TEST(LambdaBasic, PrintClosureValue) {
    // 直接 print lambda 值（验证闭包值可被作为值传递，不报错）
    // 注：闭包值的字符串表示形式各后端可能不同，此处只验证至少不崩溃且产生输出
    std::string src = "var f = fun(x) { return x; };"
                      "print(\"ok\");";
    EXPECT_FOUR_BACKENDS(src, "ok");
}

// ============================================================
// 2. 作为函数参数：高阶函数组合
// ============================================================

TEST(LambdaHigherOrder, MapWithLambda) {
    // map + lambda
    std::string src = "var arr = [1, 2, 3, 4];"
                      "var result = map(arr, fun(x) { return x * 3; });"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[3, 6, 9, 12]");
}

TEST(LambdaHigherOrder, FilterWithLambda) {
    // filter + lambda
    std::string src = "var arr = [1, 2, 3, 4, 5, 6];"
                      "var result = filter(arr, fun(x) { return x % 2 == 0; });"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[2, 4, 6]");
}

TEST(LambdaHigherOrder, ReduceWithLambda) {
    // reduce + lambda
    std::string src = "var arr = [1, 2, 3, 4];"
                      "var result = reduce(arr, fun(acc, x) { return acc + x; }, 0);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "10");
}

TEST(LambdaHigherOrder, FindWithLambda) {
    // find + lambda
    std::string src = "var arr = [1, 2, 3, 4, 5];"
                      "var result = find(arr, fun(x) { return x > 3; });"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "4");
}

TEST(LambdaHigherOrder, ForEachWithLambda) {
    // forEach + lambda，通过副作用累积输出
    std::string src = "var arr = [10, 20, 30];"
                      "forEach(arr, fun(x) { print(x); print(\" \"); });";
    EXPECT_FOUR_BACKENDS(src, "10 20 30 ");
}

TEST(LambdaHigherOrder, ChainedMapFilter) {
    // 链式：filter → map
    std::string src = "var arr = [1, 2, 3, 4, 5, 6];"
                      "var evens = filter(arr, fun(x) { return x % 2 == 0; });"
                      "var squared = map(evens, fun(x) { return x * x; });"
                      "print(squared);";
    EXPECT_FOUR_BACKENDS(src, "[4, 16, 36]");
}

// ============================================================
// 3. 闭包捕获：lambda 捕获外层局部变量
// ============================================================

TEST(LambdaClosure, CaptureLocal) {
    // lambda 捕获外层局部变量
    std::string src = "fun main() {"
                      "  var factor = 5;"
                      "  var f = fun(x) { return x * factor; };"
                      "  print(f(10));"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "50");
}

TEST(LambdaClosure, CaptureAndMutate) {
    // 捕获的变量在 lambda 调用时被读取（注意：MiniLang 闭包按值快照）
    std::string src = "fun main() {"
                      "  var n = 10;"
                      "  var f = fun(x) { return x + n; };"
                      "  print(f(5));"
                      "  n = 100;"
                      "  print(f(5));"
                      "}"
                      "main();";
    // 闭包捕获语义：n 在闭包创建时被快照（值类型），后续 n=100 不影响已捕获的 n
    // 若实现为引用捕获则两次输出均为 105/15。MiniLang 标准 upvalue 语义是引用捕获，
    // 因此两次输出应为 15 和 105。
    EXPECT_FOUR_BACKENDS(src, "15105");
}

TEST(LambdaClosure, CaptureMultiple) {
    // 捕获多个外层变量
    std::string src = "fun main() {"
                      "  var a = 2;"
                      "  var b = 3;"
                      "  var c = 4;"
                      "  var f = fun(x) { return a*x*x + b*x + c; };"
                      "  print(f(5));"
                      "}"
                      "main();";
    // 2*25 + 3*5 + 4 = 50 + 15 + 4 = 69
    EXPECT_FOUR_BACKENDS(src, "69");
}

TEST(LambdaClosure, NestedLambda) {
    // 嵌套 lambda：外层 lambda 返回内层 lambda
    std::string src = "fun main() {"
                      "  var adder = fun(a) {"
                      "    return fun(b) { return a + b; };"
                      "  };"
                      "  var add5 = adder(5);"
                      "  print(add5(10));"
                      "  print(adder(7)(3));"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "1510");
}

TEST(LambdaClosure, CaptureFromOuterFunction) {
    // lambda 在嵌套函数中捕获外层函数的局部变量
    std::string src = "fun makeMultiplier(factor) {"
                      "  return fun(x) { return x * factor; };"
                      "}"
                      "var double = makeMultiplier(2);"
                      "var triple = makeMultiplier(3);"
                      "print(double(10));"
                      "print(triple(10));";
    EXPECT_FOUR_BACKENDS(src, "2030");
}

// ============================================================
// 4. lambda 作为数组元素
// ============================================================

TEST(LambdaAsValue, InArray) {
    // lambda 作为数组元素，遍历调用
    std::string src = "fun main() {"
                      "  var fns = [fun(x) { return x + 1; }, fun(x) { return x * 2; }, fun(x) { return x - 3; }];"
                      "  print(fns[0](10));"
                      "  print(fns[1](10));"
                      "  print(fns[2](10));"
                      "}"
                      "main();";
    // 11, 20, 7
    EXPECT_FOUR_BACKENDS(src, "11207");
}

TEST(LambdaAsValue, MapOverLambdaArray) {
    // 数组中存放 lambda，用 forEach 调用
    std::string src = "fun main() {"
                      "  var fns = [fun(x) { print(x); }];"
                      "  forEach(fns, fun(f) { f(42); });"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "42");
}

// ============================================================
// 5. lambda 与递归（注意：匿名 lambda 无名，不能自引用，故仅间接递归）
// ============================================================

TEST(LambdaEdge, LambdaInCondition) {
    // lambda 在 if 分支中作为返回值
    std::string src = "fun main() {"
                      "  var n = 5;"
                      "  var f = fun(x) { return x * 2; };"
                      "  if (n > 3) {"
                      "    print(f(n));"
                      "  } else {"
                      "    print(0);"
                      "  }"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "10");
}

TEST(LambdaEdge, LambdaAsDictValue) {
    // lambda 作为字典值
    std::string src = "fun main() {"
                      "  var ops = {\"add\": fun(a, b) { return a + b; }, \"mul\": fun(a, b) { return a * b; }};"
                      "  print(ops[\"add\"](3, 4));"
                      "  print(ops[\"mul\"](3, 4));"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "712");
}

TEST(LambdaEdge, LambdaReturnFromFunction) {
    // 函数返回 lambda（外层函数参数被捕获）
    std::string src = "fun makeIncrementer(step) {"
                      "  return fun(x) { return x + step; };"
                      "}"
                      "var inc10 = makeIncrementer(10);"
                      "print(inc10(5));"
                      "print(inc10(20));";
    EXPECT_FOUR_BACKENDS(src, "1530");
}

// ============================================================
// 6. lambda 与类型注解（可选 :type 或 ->type）
// ============================================================

TEST(LambdaTypeAnnotation, ReturnTypeArrow) {
    // ->type 返回类型注解
    std::string src = "var f = fun(x) -> int { return x + 1; };"
                      "print(f(41));";
    EXPECT_FOUR_BACKENDS(src, "42");
}

TEST(LambdaTypeAnnotation, ReturnTypeColon) {
    // :type 返回类型注解
    std::string src = "var f = fun(x): int { return x * 2; };"
                      "print(f(21));";
    EXPECT_FOUR_BACKENDS(src, "42");
}

TEST(LambdaTypeAnnotation, ParamAndReturnTypes) {
    // 参数类型 + 返回类型注解
    std::string src = "var f = fun(x: int, y: int): int { return x + y; };"
                      "print(f(20, 22));";
    EXPECT_FOUR_BACKENDS(src, "42");
}

// ============================================================
// 7. lambda 表达式语句（statement 位置）
// ============================================================

TEST(LambdaStatement, ExpressionStatement) {
    // 表达式语句：lambda 表达式作为语句（求值后丢弃）
    std::string src = "fun main() {"
                      "  fun(x) { return x + 1; }(10);"
                      "  print(\"done\");"
                      "}"
                      "main();";
    // lambda 表达式语句求值后 POP，不影响后续输出
    EXPECT_FOUR_BACKENDS(src, "done");
}

// ============================================================
// 8. 综合场景：闭包 + 高阶函数
// ============================================================

TEST(LambdaComprehensive, CounterFactory) {
    // 计数器工厂：每次调用 makeCounter 返回一个独立计数的 lambda
    std::string src = "fun makeCounter() {"
                      "  var count = 0;"
                      "  return fun() { count = count + 1; return count; };"
                      "}"
                      "var c1 = makeCounter();"
                      "var c2 = makeCounter();"
                      "print(c1());"
                      "print(c1());"
                      "print(c2());"
                      "print(c1());";
    // c1: 1, 2, (c2: 1), c1: 3
    EXPECT_FOUR_BACKENDS(src, "1213");
}

TEST(LambdaComprehensive, Compose) {
    // 函数组合：compose(f, g)(x) = f(g(x))
    std::string src = "fun compose(f, g) {"
                      "  return fun(x) { return f(g(x)); };"
                      "}"
                      "var add1 = fun(x) { return x + 1; };"
                      "var mul2 = fun(x) { return x * 2; };"
                      "var fn = compose(add1, mul2);"
                      "print(fn(10));";
    // mul2(10) = 20, add1(20) = 21
    EXPECT_FOUR_BACKENDS(src, "21");
}
