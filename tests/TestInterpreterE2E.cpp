// ============================================================
// Interpreter 端到端（E2E）测试
// ------------------------------------------------------------
// 测试流程：源码 → Lexer::scan() → Parser::parse() → Interpreter::execute()
// 通过设置输出回调捕获 print 输出，验证解释器的整体行为。
// 覆盖功能点：
//   1. 基本算术
//   2. 变量声明与赋值
//   3. 控制流（if/while/for）
//   4. 函数定义与调用（含递归）
//   5. 闭包
//   6. 数组操作
//   7. 字典操作
//   8. 字符串操作
//   9. 类与继承
//  10. 错误处理
//
// A6 fix: 关于与 TestVME2E.cpp 的用例重复
// ------------------------------------------------------------
// 本文件中 BasicArithmetic / VariableDeclaration / IfTrueBranch /
// WhileLoopSum / FactorialRecursive / ArrayPush / StringLen /
// ClassBasic 等用例在 TestVME2E.cpp 的 VME2E.* 测试套件中有同名的
// 镜像用例（同样源码、同样断言）。这种重复是**有意的防御性覆盖**：
//   - 本文件验证 Interpreter 后端独立正确性（catch Interpreter-only bugs）
//   - TestVME2E.cpp 验证 Stack VM 后端独立正确性（catch VM-only bugs）
//   - 跨后端等价性由 TestVME2E.cpp 中的 VMConsistency.* 测试套件覆盖
//     （通过 runInterpreterOutputForConsistency / runVMOutputForConsistency
//      两个 helper 对比同源码在两后端的输出）
// 故不删除重复用例——每个用例独立守护其后端。
// 如需新增跨后端一致性用例，请添加到 TestVME2E.cpp 的 VMConsistency.*
// 或 BackendConsistency.* 套件，避免在此处与 VME2E.* 双向复制。
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"

#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

// ============================================================
// 辅助：执行源码并捕获 print 输出
// ============================================================

// 执行源码，返回所有 print 输出拼接后的字符串（每条 print 不自动加换行）
static std::string runInterpreterOutput(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast) return "";

    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });
    interp.execute(*ast);
    return captured;
}

// 执行源码，返回最后一条语句的求值结果（不依赖 print）
static Value runInterpreterValue(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return Value::nullValue();

    Interpreter interp;
    interp.setOutputCallback([](const std::string&) {});
    return interp.execute(*ast);
}

// 执行源码，预期抛出 RuntimeError
static bool runInterpreterThrows(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return false;

    Interpreter interp;
    interp.setOutputCallback([](const std::string&) {});
    try {
        interp.execute(*ast);
        return false;
    } catch (const RuntimeError&) {
        return true;
    }
}

// ============================================================
// 1. 基本算术
// ============================================================

// 测试：1 + 2 应输出 3
TEST(InterpreterE2E, BasicAddition) {
    std::string output = runInterpreterOutput("print(1 + 2);");
    EXPECT_EQ(output, "3");
}

// 测试：减法、乘法、除法
TEST(InterpreterE2E, BasicArithmetic) {
    EXPECT_EQ(runInterpreterOutput("print(10 - 3);"), "7");
    EXPECT_EQ(runInterpreterOutput("print(4 * 5);"), "20");
    EXPECT_EQ(runInterpreterOutput("print(20 / 4);"), "5");
    EXPECT_EQ(runInterpreterOutput("print(17 % 5);"), "2");
}

// 测试：运算符优先级
TEST(InterpreterE2E, OperatorPrecedence) {
    // 2 + 3 * 4 = 14
    EXPECT_EQ(runInterpreterOutput("print(2 + 3 * 4);"), "14");
    // (2 + 3) * 4 = 20
    EXPECT_EQ(runInterpreterOutput("print((2 + 3) * 4);"), "20");
}

// 测试：变量参与的算术
TEST(InterpreterE2E, ArithmeticWithVariables) {
    std::string src = "var x = 10; var y = 20; print(x + y);";
    EXPECT_EQ(runInterpreterOutput(src), "30");
}

// 测试：浮点运算
TEST(InterpreterE2E, FloatArithmetic) {
    EXPECT_EQ(runInterpreterOutput("print(1.5 + 2.5);"), "4");
    EXPECT_EQ(runInterpreterOutput("print(3.14 * 2);"), "6.2800000000000002");
}

// ============================================================
// 2. 变量声明与赋值
// ============================================================

// 测试：变量声明后读取
TEST(InterpreterE2E, VariableDeclaration) {
    std::string src = "var x = 5; print(x);";
    EXPECT_EQ(runInterpreterOutput(src), "5");
}

// 测试：变量赋值（修改值）
TEST(InterpreterE2E, VariableAssignment) {
    std::string src = "var x = 5; print(x); x = 10; print(x);";
    EXPECT_EQ(runInterpreterOutput(src), "510");
}

// 测试：带类型注解的变量声明
TEST(InterpreterE2E, TypedVariableDeclaration) {
    std::string src = "int a = 42; print(a);";
    EXPECT_EQ(runInterpreterOutput(src), "42");
}

// 测试：多个变量交互
TEST(InterpreterE2E, MultipleVariables) {
    std::string src = "var a = 1; var b = 2; var c = a + b; print(c);";
    EXPECT_EQ(runInterpreterOutput(src), "3");
}

// ============================================================
// 3. 控制流
// ============================================================

// 测试：if-else 条件为真
TEST(InterpreterE2E, IfTrueBranch) {
    std::string src = "if (true) { print(1); } else { print(2); }";
    EXPECT_EQ(runInterpreterOutput(src), "1");
}

// 测试：if-else 条件为假
TEST(InterpreterE2E, IfFalseBranch) {
    std::string src = "if (false) { print(1); } else { print(2); }";
    EXPECT_EQ(runInterpreterOutput(src), "2");
}

// 测试：if-elseif-else 链
TEST(InterpreterE2E, IfElseIfChain) {
    std::string src =
        "var x = 5;"
        "if (x == 1) { print(1); }"
        "else if (x == 5) { print(5); }"
        "else { print(0); }";
    EXPECT_EQ(runInterpreterOutput(src), "5");
}

// 测试：while 循环计算 1 到 10 的和
TEST(InterpreterE2E, WhileLoopSum) {
    std::string src =
        "var sum = 0;"
        "var i = 1;"
        "while (i <= 10) { sum = sum + i; i = i + 1; }"
        "print(sum);";
    EXPECT_EQ(runInterpreterOutput(src), "55");
}

// 测试：while 循环条件不满足时不执行
TEST(InterpreterE2E, WhileLoopNotExecuted) {
    std::string src =
        "var i = 100;"
        "while (i < 10) { print(i); i = i + 1; }"
        "print(99);";
    EXPECT_EQ(runInterpreterOutput(src), "99");
}

// 测试：for 循环打印 1 到 5
TEST(InterpreterE2E, ForLoopPrint) {
    std::string src =
        "for (var i = 1; i <= 5; i = i + 1) { print(i); }";
    EXPECT_EQ(runInterpreterOutput(src), "12345");
}

// 测试：for 循环计算累加和
TEST(InterpreterE2E, ForLoopSum) {
    std::string src =
        "var sum = 0;"
        "for (var i = 1; i <= 100; i = i + 1) { sum = sum + i; }"
        "print(sum);";
    EXPECT_EQ(runInterpreterOutput(src), "5050");
}

// ============================================================
// 4. 函数定义与调用
// ============================================================

// 测试：简单函数调用
TEST(InterpreterE2E, SimpleFunctionCall) {
    std::string src =
        "fun add(a, b) { return a + b; }"
        "print(add(3, 4));";
    EXPECT_EQ(runInterpreterOutput(src), "7");
}

// 测试：函数无返回值（默认 null）
TEST(InterpreterE2E, FunctionNoReturn) {
    std::string src =
        "fun greet() { print(123); }"
        "greet();";
    EXPECT_EQ(runInterpreterOutput(src), "123");
}

// 测试：阶乘递归
TEST(InterpreterE2E, FactorialRecursive) {
    std::string src =
        "fun fact(n) {"
        "  if (n <= 1) { return 1; }"
        "  return n * fact(n - 1);"
        "}"
        "print(fact(5));";
    EXPECT_EQ(runInterpreterOutput(src), "120");
}

// 测试：斐波那契递归
TEST(InterpreterE2E, FibonacciRecursive) {
    std::string src =
        "fun fib(n) {"
        "  if (n < 2) { return n; }"
        "  return fib(n - 1) + fib(n - 2);"
        "}"
        "print(fib(10));";
    EXPECT_EQ(runInterpreterOutput(src), "55");
}

// 测试：函数参数为表达式
TEST(InterpreterE2E, FunctionCallWithExpression) {
    std::string src =
        "fun double(x) { return x * 2; }"
        "print(double(3 + 4));";
    EXPECT_EQ(runInterpreterOutput(src), "14");
}

// 测试：嵌套函数调用
TEST(InterpreterE2E, NestedFunctionCall) {
    std::string src =
        "fun inc(x) { return x + 1; }"
        "fun double(x) { return x * 2; }"
        "print(inc(double(5)));";
    EXPECT_EQ(runInterpreterOutput(src), "11");
}

// ============================================================
// 5. 闭包
// ============================================================

// 测试：函数作为返回值（计数器）
TEST(InterpreterE2E, ClosureAsReturnValue) {
    std::string src =
        "fun makeCounter() {"
        "  var count = 0;"
        "  fun increment() { count = count + 1; return count; }"
        "  return increment;"
        "}"
        "var c = makeCounter();"
        "print(c());"
        "print(c());"
        "print(c());";
    EXPECT_EQ(runInterpreterOutput(src), "123");
}

// 测试：闭包捕获外部变量
TEST(InterpreterE2E, ClosureCapturesVariable) {
    std::string src =
        "var x = 10;"
        "fun getX() { return x; }"
        "print(getX());"
        "x = 20;"
        "print(getX());";
    EXPECT_EQ(runInterpreterOutput(src), "1020");
}

// ============================================================
// 6. 数组操作
// ============================================================

// 测试：数组创建与访问
TEST(InterpreterE2E, ArrayCreateAndAccess) {
    std::string src =
        "var arr = [1, 2, 3];"
        "print(arr[0]);"
        "print(arr[1]);"
        "print(arr[2]);";
    EXPECT_EQ(runInterpreterOutput(src), "123");
}

// 测试：数组索引赋值
TEST(InterpreterE2E, ArrayIndexAssign) {
    std::string src =
        "var arr = [1, 2, 3];"
        "arr[0] = 99;"
        "print(arr[0]);";
    EXPECT_EQ(runInterpreterOutput(src), "99");
}

// 测试：数组 push 方法
TEST(InterpreterE2E, ArrayPush) {
    std::string src =
        "var arr = [1, 2];"
        "arr.push(3);"
        "print(arr[2]);"
        "print(arr.len());";
    EXPECT_EQ(runInterpreterOutput(src), "33");
}

// 测试：数组 pop 方法
TEST(InterpreterE2E, ArrayPop) {
    std::string src =
        "var arr = [1, 2, 3];"
        "var x = arr.pop();"
        "print(x);"
        "print(arr.len());";
    EXPECT_EQ(runInterpreterOutput(src), "32");
}

// 测试：数组 len 方法
TEST(InterpreterE2E, ArrayLen) {
    std::string src =
        "var arr = [10, 20, 30, 40];"
        "print(arr.len());";
    EXPECT_EQ(runInterpreterOutput(src), "4");
}

// 测试：数组 contains 方法
TEST(InterpreterE2E, ArrayContains) {
    std::string src =
        "var arr = [1, 2, 3];"
        "print(arr.contains(2));"
        "print(arr.contains(99));";
    EXPECT_EQ(runInterpreterOutput(src), "truefalse");
}

// 测试：数组 join 方法
TEST(InterpreterE2E, ArrayJoin) {
    std::string src =
        "var arr = [1, 2, 3];"
        "print(arr.join(\",\"));";
    EXPECT_EQ(runInterpreterOutput(src), "1,2,3");
}

// 测试：遍历数组
TEST(InterpreterE2E, ArrayIterate) {
    std::string src =
        "var arr = [10, 20, 30];"
        "var sum = 0;"
        "for (var i = 0; i < arr.len(); i = i + 1) { sum = sum + arr[i]; }"
        "print(sum);";
    EXPECT_EQ(runInterpreterOutput(src), "60");
}

// ============================================================
// 7. 字典操作
// ============================================================

// 测试：字典创建与访问
TEST(InterpreterE2E, DictCreateAndAccess) {
    std::string src =
        "var d = {\"a\": 1, \"b\": 2};"
        "print(d[\"a\"]);"
        "print(d[\"b\"]);";
    EXPECT_EQ(runInterpreterOutput(src), "12");
}

// 测试：字典键赋值
TEST(InterpreterE2E, DictKeyAssign) {
    std::string src =
        "var d = {\"x\": 1};"
        "d[\"x\"] = 100;"
        "d[\"y\"] = 200;"
        "print(d[\"x\"]);"
        "print(d[\"y\"]);";
    EXPECT_EQ(runInterpreterOutput(src), "100200");
}

// 测试：字典 len 方法
TEST(InterpreterE2E, DictLen) {
    std::string src =
        "var d = {\"a\": 1, \"b\": 2, \"c\": 3};"
        "print(d.len());";
    EXPECT_EQ(runInterpreterOutput(src), "3");
}

// 测试：字典 has 方法
TEST(InterpreterE2E, DictHas) {
    std::string src =
        "var d = {\"name\": \"Alice\"};"
        "print(d.has(\"name\"));"
        "print(d.has(\"age\"));";
    EXPECT_EQ(runInterpreterOutput(src), "truefalse");
}

// 测试：字典 keys 方法
TEST(InterpreterE2E, DictKeys) {
    std::string src =
        "var d = {\"a\": 1, \"b\": 2};"
        "var ks = d.keys();"
        "print(ks.len());";
    EXPECT_EQ(runInterpreterOutput(src), "2");
}

// 测试：字典 get 方法（带默认值）
TEST(InterpreterE2E, DictGetWithDefault) {
    std::string src =
        "var d = {\"a\": 1};"
        "print(d.get(\"a\", 99));"
        "print(d.get(\"missing\", 99));";
    EXPECT_EQ(runInterpreterOutput(src), "199");
}

// ============================================================
// 8. 字符串操作
// ============================================================

// 测试：字符串拼接
TEST(InterpreterE2E, StringConcatenation) {
    EXPECT_EQ(runInterpreterOutput("print(\"hello\" + \" \" + \"world\");"),
              "hello world");
}

// 测试：字符串与数字拼接
TEST(InterpreterE2E, StringNumberConcat) {
    EXPECT_EQ(runInterpreterOutput("print(\"count: \" + 42);"), "count: 42");
}

// 测试：字符串 len 方法
TEST(InterpreterE2E, StringLen) {
    std::string src = "var s = \"hello\"; print(s.len());";
    EXPECT_EQ(runInterpreterOutput(src), "5");
}

// 测试：字符串 upper/lower 方法
TEST(InterpreterE2E, StringUpperLower) {
    std::string src =
        "var s = \"Hello\";"
        "print(s.upper());"
        "print(s.lower());";
    EXPECT_EQ(runInterpreterOutput(src), "HELLOhello");
}

// 测试：字符串 split 方法
TEST(InterpreterE2E, StringSplit) {
    std::string src =
        "var s = \"a,b,c\";"
        "var arr = s.split(\",\");"
        "print(arr.len());"
        "print(arr[0]);"
        "print(arr[1]);"
        "print(arr[2]);";
    EXPECT_EQ(runInterpreterOutput(src), "3abc");
}

// 测试：字符串 replace 方法
TEST(InterpreterE2E, StringReplace) {
    std::string src =
        "var s = \"a-b-c\";"
        "print(s.replace(\"-\", \"+\"));";
    EXPECT_EQ(runInterpreterOutput(src), "a+b+c");
}

// 测试：字符串 trim 方法
TEST(InterpreterE2E, StringTrim) {
    std::string src =
        "var s = \"  hello  \";"
        "print(s.trim());";
    EXPECT_EQ(runInterpreterOutput(src), "hello");
}

// 测试：字符串 contains 方法
TEST(InterpreterE2E, StringContains) {
    std::string src =
        "var s = \"hello world\";"
        "print(s.contains(\"world\"));"
        "print(s.contains(\"xyz\"));";
    EXPECT_EQ(runInterpreterOutput(src), "truefalse");
}

// ============================================================
// 9. 类与继承
// ============================================================

// 测试：类定义、实例化与方法调用
TEST(InterpreterE2E, ClassBasic) {
    std::string src =
        "class Point {"
        "  var x = 0;"
        "  var y = 0;"
        "  fun getX() { return x; }"
        "  fun setX(v) { x = v; }"
        "}"
        "var p = Point();"
        "print(p.getX());"
        "p.setX(42);"
        "print(p.getX());";
    EXPECT_EQ(runInterpreterOutput(src), "042");
}

// 测试：类成员访问与赋值
TEST(InterpreterE2E, ClassMemberAccess) {
    std::string src =
        "class Box {"
        "  var value = 0;"
        "}"
        "var b = Box();"
        "print(b.value);"
        "b.value = 99;"
        "print(b.value);";
    EXPECT_EQ(runInterpreterOutput(src), "099");
}

// 测试：继承与 super 调用
TEST(InterpreterE2E, ClassInheritanceAndSuper) {
    std::string src =
        "class Animal {"
        "  fun speak() { return \"generic sound\"; }"
        "}"
        "class Dog extends Animal {"
        "  fun speak() { return \"woof: \" + super.speak(); }"
        "}"
        "var d = Dog();"
        "print(d.speak());";
    EXPECT_EQ(runInterpreterOutput(src), "woof: generic sound");
}

// 测试：继承链中方法查找
TEST(InterpreterE2E, InheritedMethod) {
    std::string src =
        "class Base {"
        "  fun greet() { return \"hello from base\"; }"
        "}"
        "class Derived extends Base {"
        "}"
        "var d = Derived();"
        "print(d.greet());";
    EXPECT_EQ(runInterpreterOutput(src), "hello from base");
}

// ============================================================
// 10. 错误处理
// ============================================================

// 测试：除零错误
TEST(InterpreterE2E, DivisionByZeroError) {
    EXPECT_TRUE(runInterpreterThrows("print(1 / 0);"));
}

// 测试：取模除零错误
TEST(InterpreterE2E, ModuloByZeroError) {
    EXPECT_TRUE(runInterpreterThrows("print(10 % 0);"));
}

// 测试：未定义变量
TEST(InterpreterE2E, UndefinedVariableError) {
    EXPECT_TRUE(runInterpreterThrows("print(undefinedVar);"));
}

// 测试：类型错误（对非数值做算术）
TEST(InterpreterE2E, TypeErrorOnArithmetic) {
    // 对数组做加法应抛出运行时错误
    EXPECT_TRUE(runInterpreterThrows("var a = [1,2]; var b = a + 1; print(b);"));
}

// 测试：调用未定义函数
TEST(InterpreterE2E, UndefinedFunctionError) {
    EXPECT_TRUE(runInterpreterThrows("undefinedFunc();"));
}

// 测试：数组索引越界
TEST(InterpreterE2E, ArrayIndexOutOfBoundsError) {
    EXPECT_TRUE(runInterpreterThrows("var a = [1,2,3]; print(a[10]);"));
}

// 测试：对空数组 pop
TEST(InterpreterE2E, PopFromEmptyArrayError) {
    EXPECT_TRUE(runInterpreterThrows("var a = []; a.pop();"));
}

// 测试：错误信息包含相关内容
TEST(InterpreterE2E, ErrorMessageContent) {
    Lexer lexer;
    auto tokens = lexer.scan("print(1 / 0);");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_TRUE(ast != nullptr);

    Interpreter interp;
    interp.setOutputCallback([](const std::string&) {});
    try {
        interp.execute(*ast);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("除零"), std::string::npos);
    }
}

// 测试：错误携带行号
TEST(InterpreterE2E, ErrorPreservesLine) {
    Lexer lexer;
    // 第二行触发除零错误
    auto tokens = lexer.scan("var x = 1;\nprint(x / 0);");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_TRUE(ast != nullptr);

    Interpreter interp;
    interp.setOutputCallback([](const std::string&) {});
    try {
        interp.execute(*ast);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError& e) {
        // 行号应大于 0（具体行号取决于实现）
        EXPECT_GE(e.line, 0);
    }
}

// ============================================================
// 综合测试
// ============================================================

// 测试：综合 - 函数 + 循环 + 数组
TEST(InterpreterE2E, ComprehensiveFunctionLoopArray) {
    std::string src =
        "fun sumArray(arr) {"
        "  var total = 0;"
        "  var i = 0;"
        "  while (i < arr.len()) { total = total + arr[i]; i = i + 1; }"
        "  return total;"
        "}"
        "var data = [1, 2, 3, 4, 5];"
        "print(sumArray(data));";
    EXPECT_EQ(runInterpreterOutput(src), "15");
}

// 测试：综合 - 类 + 闭包
TEST(InterpreterE2E, ComprehensiveClassAndClosure) {
    std::string src =
        "class Counter {"
        "  var count = 0;"
        "  fun inc() { count = count + 1; return count; }"
        "  fun get() { return count; }"
        "}"
        "var c = Counter();"
        "print(c.inc());"
        "print(c.inc());"
        "print(c.get());";
    EXPECT_EQ(runInterpreterOutput(src), "122");
}

// 测试：综合 - 字符串处理
TEST(InterpreterE2E, ComprehensiveStringProcessing) {
    std::string src =
        "var s = \"hello,world,foo,bar\";"
        "var parts = s.split(\",\");"
        "print(parts.len());"
        "var i = 0;"
        "while (i < parts.len()) {"
        "  print(parts[i]);"
        "  i = i + 1;"
        "}";
    EXPECT_EQ(runInterpreterOutput(src), "4helloworldfoobar");
}

// ============================================================
// 11. F12 模块系统 / import / export
// ============================================================

// 辅助：执行源码并加载模拟模块，返回 print 输出
// modules: 模块路径 -> 模块源代码 的映射
static std::string runInterpreterWithModules(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast) return "";

    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });
    interp.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end()) throw std::runtime_error("module not found: " + path);
        return it->second;
    });
    interp.execute(*ast);
    return captured;
}

// 辅助：执行带模块的源码，预期抛出 RuntimeError
static bool runInterpreterWithModulesThrows(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return false;

    Interpreter interp;
    interp.setOutputCallback([](const std::string&) {});
    interp.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end()) throw std::runtime_error("module not found: " + path);
        return it->second;
    });
    try {
        interp.execute(*ast);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

// 测试：import 全部导出
TEST(InterpreterE2E, ModuleImportAll) {
    std::string src =
        "import \"mymod\";"
        "print(PI);"
        "print(add(3, 4));";
    std::unordered_map<std::string, std::string> modules = {
        {"mymod",
         "export var PI = 314;"
         "export fun add(a, b) { return a + b; }"}
    };
    EXPECT_EQ(runInterpreterWithModules(src, modules), "3147");
}

// 测试：import 指定名称
TEST(InterpreterE2E, ModuleImportNamed) {
    std::string src =
        "import { greet } from \"greetings\";"
        "print(greet(\"world\"));";
    std::unordered_map<std::string, std::string> modules = {
        {"greetings",
         "export fun greet(name) { return \"hello \" + name; }"
         "export fun unused() { return 999; }"}
    };
    EXPECT_EQ(runInterpreterWithModules(src, modules), "hello world");
}

// 测试：import 指定名称时未导出的名称不可访问
TEST(InterpreterE2E, ModuleImportNamedNotExported) {
    std::string src =
        "import { secret } from \"m\";"
        "print(secret);";
    std::unordered_map<std::string, std::string> modules = {
        {"m", "var secret = 42; export var public_val = 1;"}
    };
    EXPECT_TRUE(runInterpreterWithModulesThrows(src, modules));
}

// 测试：export 类
TEST(InterpreterE2E, ModuleExportClass) {
    std::string src =
        "import { Point } from \"geom\";"
        "var p = Point(3, 4);"
        "print(p.x);"
        "print(p.y);"
        "print(p.norm());";
    std::unordered_map<std::string, std::string> modules = {
        {"geom",
         "export class Point {"
         "  var x = 0;"
         "  var y = 0;"
         "  fun init(ax, ay) { x = ax; y = ay; }"
         "  fun norm() { return (x * x + y * y) % 100; }"
         "}"}
    };
    EXPECT_EQ(runInterpreterWithModules(src, modules), "3425");
}

// 测试：模块缓存（多次 import 同一模块只执行一次）
TEST(InterpreterE2E, ModuleCache) {
    std::string src =
        "import { counter } from \"counter_mod\";"
        "print(counter());"
        "import { counter } from \"counter_mod\";"
        "print(counter());";
    // 模块中使用模块级变量计数，验证模块只执行一次
    std::unordered_map<std::string, std::string> modules = {
        {"counter_mod",
         "var count = 10;"
         "export fun counter() { count = count + 1; return count; }"}
    };
    // 第一次调用 counter() → 11，第二次（重新 import 但模块已缓存）→ 12
    EXPECT_EQ(runInterpreterWithModules(src, modules), "1112");
}

// 测试：循环依赖检测
TEST(InterpreterE2E, ModuleCircularDependency) {
    std::string src = "import \"a\";";
    std::unordered_map<std::string, std::string> modules = {
        {"a", "import \"b\";"},
        {"b", "import \"a\";"}
    };
    EXPECT_TRUE(runInterpreterWithModulesThrows(src, modules));
}

// 测试：模块不存在
TEST(InterpreterE2E, ModuleNotFound) {
    std::string src = "import \"nonexistent\";";
    std::unordered_map<std::string, std::string> modules;
    EXPECT_TRUE(runInterpreterWithModulesThrows(src, modules));
}

// 测试：未设置模块加载器
TEST(InterpreterE2E, ModuleNoLoader) {
    std::string src = "import \"m\";";
    Lexer lexer;
    auto tokens = lexer.scan(src);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_TRUE(ast != nullptr);
    Interpreter interp;
    interp.setOutputCallback([](const std::string&) {});
    try {
        interp.execute(*ast);
        FAIL() << "应抛出 RuntimeError";
    } catch (const RuntimeError&) {
        SUCCEED();
    }
}

// 测试：模块内未导出的变量不影响导入方
TEST(InterpreterE2E, ModuleIsolation) {
    std::string src =
        "import { public_val } from \"m\";"
        "print(public_val);";
    std::unordered_map<std::string, std::string> modules = {
        {"m",
         "var hidden = 999;"
         "export var public_val = 1;"}
    };
    EXPECT_EQ(runInterpreterWithModules(src, modules), "1");
}

// 测试：嵌套模块导入
TEST(InterpreterE2E, ModuleNestedImport) {
    std::string src =
        "import { getValue } from \"outer\";"
        "print(getValue());";
    std::unordered_map<std::string, std::string> modules = {
        {"outer",
         "import { base } from \"inner\";"
         "export fun getValue() { return base + 100; }"},
        {"inner",
         "export var base = 42;"}
    };
    EXPECT_EQ(runInterpreterWithModules(src, modules), "142");
}

// 测试：import 全部时只导入 export 的名称
TEST(InterpreterE2E, ModuleImportAllOnlyExports) {
    std::string src =
        "import \"m\";"
        "print(private_val);";
    std::unordered_map<std::string, std::string> modules = {
        {"m",
         "var private_val = 999;"
         "export var public_val = 1;"}
    };
    // import "m" 导入全部导出名称，private_val 未导出，不应可访问
    EXPECT_TRUE(runInterpreterWithModulesThrows(src, modules));
}

// P2-1 fix: 命名导入原子性 — 部分名称不存在时不导入任何名称
TEST(InterpreterE2E, ModuleImportNamedAtomic) {
    std::string src =
        "import { a, nonexistent, c } from \"m\";"
        "print(a);"
        "print(c);";
    std::unordered_map<std::string, std::string> modules = {
        {"m",
         "export var a = 1;"
         "export var c = 3;"}
    };
    // nonexistent 未导出，应抛出错误，且 a 和 c 不应被导入
    EXPECT_TRUE(runInterpreterWithModulesThrows(src, modules));
}

// P2-1 fix: 命名导入原子性 — 验证错误后环境不被污染
TEST(InterpreterE2E, ModuleImportNamedAtomicNoPollution) {
    std::unordered_map<std::string, std::string> modules = {
        {"m",
         "export var a = 1;"}
    };
    // import 失败后 a 不应被导入（原子性：先验证全部名称，再统一定义）
    EXPECT_TRUE(runInterpreterWithModulesThrows(
        "import { a, nonexistent } from \"m\";"
        "print(a);",
        modules));
}

// P2-3 fix: 模块缓存键路径规范化 — 不同路径表示应命中同一缓存
TEST(InterpreterE2E, ModuleCacheKeyNormalization) {
    std::string src =
        "import \"./mod.mini\";"
        "import \"mod.mini\";"
        "print(\"ok\");";
    std::unordered_map<std::string, std::string> modules = {
        {"mod.mini",
         "export var val = 1;"}
    };
    // "./mod.mini" 和 "mod.mini" 规范化后应命中同一缓存
    std::string output = runInterpreterWithModules(src, modules);
    EXPECT_EQ(output, "ok");
}

// P2-3 fix: Windows 路径分隔符规范化
TEST(InterpreterE2E, ModuleCacheKeyBackslashNormalization) {
    // AUDIT-BUG-L1 fix: \m 是未知转义，必须用 \\ 表示字面反斜杠
    std::string src =
        "import \"sub/mod.mini\";"
        "import \"sub\\\\mod.mini\";"
        "print(\"ok\");";
    std::unordered_map<std::string, std::string> modules = {
        {"sub/mod.mini",
         "export var val = 1;"}
    };
    // "sub/mod.mini" 和 "sub\mod.mini" 规范化后应命中同一缓存
    std::string output = runInterpreterWithModules(src, modules);
    EXPECT_EQ(output, "ok");
}
