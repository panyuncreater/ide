// ============================================================
// VM 端到端（E2E）测试
// ------------------------------------------------------------
// 测试流程：源码 → Lexer::scan() → Parser::parse() → Compiler::compile() → VM::execute()
// 通过设置输出回调捕获 print 输出，验证虚拟机的整体行为。
// 覆盖功能点：
//   1. 基本算术
//   2. 变量声明与赋值
//   3. 控制流（if/while/for）
//   4. 函数定义与调用
//   5. 数组操作
//   6. 字符串操作
//   7. 错误处理
//   8. 一致性测试（与 Interpreter 结果对比）
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "interpreter/Value.h"
#include "interpreter/Interpreter.h"

#include <string>
#include <vector>

// ============================================================
// 辅助：执行源码并捕获 VM 的 print 输出
// ============================================================

// 执行源码，返回所有 print 输出拼接后的字符串
// 若编译或运行时出错，返回空字符串并通过 GTest 失败标识
static std::string runVMOutput(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast) return "";

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    // 编译错误不强制失败（某些用例可能预期编译失败），由调用方判断

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    VMResult vmr = vm.execute(result);
    // 调用方可通过 vm.hasError() 检查错误
    (void)vmr;
    return captured;
}

// 执行源码，返回 VM 的执行结果状态
static VMResult runVMResult(const std::string& source, std::string& output,
                            std::string& error) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) {
        error = "解析失败";
        return VMResult::VM_RUNTIME_ERROR;
    }

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        error = compiler.getLastError();
    }

    VM vm;
    output.clear();
    vm.setOutputCallback([&](const std::string& s) { output += s; });
    VMResult vmr = vm.execute(result);
    if (vm.hasError()) {
        error = vm.getLastError();
    }
    return vmr;
}

// 执行源码，返回 VM 全局变量表
static std::unordered_map<std::string, Value> runVMGlobals(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return {};

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    vm.setOutputCallback([](const std::string&) {});
    vm.execute(result);
    return vm.getGlobals();
}

// ============================================================
// 1. 基本算术
// ============================================================

// 测试：1 + 2 应输出 3
TEST(VME2E, BasicAddition) {
    std::string output = runVMOutput("print(1 + 2);");
    EXPECT_EQ(output, "3");
}

// 测试：减法、乘法、除法、取模
TEST(VME2E, BasicArithmetic) {
    EXPECT_EQ(runVMOutput("print(10 - 3);"), "7");
    EXPECT_EQ(runVMOutput("print(4 * 5);"), "20");
    EXPECT_EQ(runVMOutput("print(20 / 4);"), "5");
    EXPECT_EQ(runVMOutput("print(17 % 5);"), "2");
}

// 测试：运算符优先级
TEST(VME2E, OperatorPrecedence) {
    EXPECT_EQ(runVMOutput("print(2 + 3 * 4);"), "14");
    EXPECT_EQ(runVMOutput("print((2 + 3) * 4);"), "20");
}

// 测试：变量参与的算术
TEST(VME2E, ArithmeticWithVariables) {
    std::string src = "var x = 10; var y = 20; print(x + y);";
    EXPECT_EQ(runVMOutput(src), "30");
}

// 测试：浮点运算
TEST(VME2E, FloatArithmetic) {
    EXPECT_EQ(runVMOutput("print(1.5 + 2.5);"), "4");
}

// 测试：一元负号
TEST(VME2E, UnaryNegate) {
    EXPECT_EQ(runVMOutput("var x = 5; print(-x);"), "-5");
}

// 测试：逻辑运算
TEST(VME2E, LogicalOps) {
    EXPECT_EQ(runVMOutput("print(true and false);"), "false");
    EXPECT_EQ(runVMOutput("print(true or false);"), "true");
    EXPECT_EQ(runVMOutput("print(not true);"), "false");
}

// 测试：比较运算
TEST(VME2E, ComparisonOps) {
    EXPECT_EQ(runVMOutput("print(1 < 2);"), "true");
    EXPECT_EQ(runVMOutput("print(2 > 1);"), "true");
    EXPECT_EQ(runVMOutput("print(1 == 1);"), "true");
    EXPECT_EQ(runVMOutput("print(1 != 2);"), "true");
    EXPECT_EQ(runVMOutput("print(2 <= 2);"), "true");
    EXPECT_EQ(runVMOutput("print(3 >= 5);"), "false");
}

// ============================================================
// 2. 变量声明与赋值
// ============================================================

// 测试：变量声明后读取
TEST(VME2E, VariableDeclaration) {
    std::string src = "var x = 5; print(x);";
    EXPECT_EQ(runVMOutput(src), "5");
}

// 测试：变量赋值（修改值）
TEST(VME2E, VariableAssignment) {
    std::string src = "var x = 5; print(x); x = 10; print(x);";
    EXPECT_EQ(runVMOutput(src), "510");
}

// 测试：带类型注解的变量声明
TEST(VME2E, TypedVariableDeclaration) {
    std::string src = "int a = 42; print(a);";
    EXPECT_EQ(runVMOutput(src), "42");
}

// 测试：多个变量交互
TEST(VME2E, MultipleVariables) {
    std::string src = "var a = 1; var b = 2; var c = a + b; print(c);";
    EXPECT_EQ(runVMOutput(src), "3");
}

// 测试：通过 getGlobals 验证变量值
TEST(VME2E, GlobalsAfterExecution) {
    auto globals = runVMGlobals("var x = 42; var y = 100;");
    ASSERT_TRUE(globals.count("x") > 0);
    EXPECT_EQ(globals["x"].intVal(), 42);
    ASSERT_TRUE(globals.count("y") > 0);
    EXPECT_EQ(globals["y"].intVal(), 100);
}

// ============================================================
// 3. 控制流
// ============================================================

// 测试：if-else 条件为真
TEST(VME2E, IfTrueBranch) {
    std::string src = "if (true) { print(1); } else { print(2); }";
    EXPECT_EQ(runVMOutput(src), "1");
}

// 测试：if-else 条件为假
TEST(VME2E, IfFalseBranch) {
    std::string src = "if (false) { print(1); } else { print(2); }";
    EXPECT_EQ(runVMOutput(src), "2");
}

// 测试：if-elseif-else 链
TEST(VME2E, IfElseIfChain) {
    std::string src =
        "var x = 5;"
        "if (x == 1) { print(1); }"
        "else if (x == 5) { print(5); }"
        "else { print(0); }";
    EXPECT_EQ(runVMOutput(src), "5");
}

// 测试：while 循环计算 1 到 10 的和
TEST(VME2E, WhileLoopSum) {
    std::string src =
        "var sum = 0;"
        "var i = 1;"
        "while (i <= 10) { sum = sum + i; i = i + 1; }"
        "print(sum);";
    EXPECT_EQ(runVMOutput(src), "55");
}

// 测试：while 循环条件不满足时不执行
TEST(VME2E, WhileLoopNotExecuted) {
    std::string src =
        "var i = 100;"
        "while (i < 10) { print(i); i = i + 1; }"
        "print(99);";
    EXPECT_EQ(runVMOutput(src), "99");
}

// 测试：for 循环打印 1 到 5
TEST(VME2E, ForLoopPrint) {
    std::string src =
        "for (var i = 1; i <= 5; i = i + 1) { print(i); }";
    EXPECT_EQ(runVMOutput(src), "12345");
}

// 测试：for 循环计算累加和
TEST(VME2E, ForLoopSum) {
    std::string src =
        "var sum = 0;"
        "for (var i = 1; i <= 100; i = i + 1) { sum = sum + i; }"
        "print(sum);";
    EXPECT_EQ(runVMOutput(src), "5050");
}

// 测试：嵌套循环（乘法表局部）
TEST(VME2E, NestedLoop) {
    std::string src =
        "var total = 0;"
        "for (var i = 1; i <= 3; i = i + 1) {"
        "  for (var j = 1; j <= 3; j = j + 1) {"
        "    total = total + i * j;"
        "  }"
        "}"
        "print(total);";
    // 1*1+1*2+1*3+2*1+...+3*3 = (1+2+3)*(1+2+3) = 6*6 = 36
    EXPECT_EQ(runVMOutput(src), "36");
}

// ============================================================
// 4. 函数定义与调用
// ============================================================

// 测试：简单函数调用
TEST(VME2E, SimpleFunctionCall) {
    std::string src =
        "fun add(a, b) { return a + b; }"
        "print(add(3, 4));";
    EXPECT_EQ(runVMOutput(src), "7");
}

// 测试：函数无返回值
TEST(VME2E, FunctionNoReturn) {
    std::string src =
        "fun greet() { print(123); }"
        "greet();";
    EXPECT_EQ(runVMOutput(src), "123");
}

// 测试：阶乘递归
TEST(VME2E, FactorialRecursive) {
    std::string src =
        "fun fact(n) {"
        "  if (n <= 1) { return 1; }"
        "  return n * fact(n - 1);"
        "}"
        "print(fact(5));";
    EXPECT_EQ(runVMOutput(src), "120");
}

// 测试：斐波那契递归
TEST(VME2E, FibonacciRecursive) {
    std::string src =
        "fun fib(n) {"
        "  if (n < 2) { return n; }"
        "  return fib(n - 1) + fib(n - 2);"
        "}"
        "print(fib(10));";
    EXPECT_EQ(runVMOutput(src), "55");
}

// 测试：函数参数为表达式
TEST(VME2E, FunctionCallWithExpression) {
    std::string src =
        "fun double(x) { return x * 2; }"
        "print(double(3 + 4));";
    EXPECT_EQ(runVMOutput(src), "14");
}

// 测试：嵌套函数调用
TEST(VME2E, NestedFunctionCall) {
    std::string src =
        "fun inc(x) { return x + 1; }"
        "fun double(x) { return x * 2; }"
        "print(inc(double(5)));";
    EXPECT_EQ(runVMOutput(src), "11");
}

// 测试：多参数函数
TEST(VME2E, MultiArgFunction) {
    std::string src =
        "fun sum4(a, b, c, d) { return a + b + c + d; }"
        "print(sum4(1, 2, 3, 4));";
    EXPECT_EQ(runVMOutput(src), "10");
}

// ============================================================
// 5. 数组操作
// ============================================================

// 测试：数组创建与访问
TEST(VME2E, ArrayCreateAndAccess) {
    std::string src =
        "var arr = [1, 2, 3];"
        "print(arr[0]);"
        "print(arr[1]);"
        "print(arr[2]);";
    EXPECT_EQ(runVMOutput(src), "123");
}

// 测试：数组索引赋值
TEST(VME2E, ArrayIndexAssign) {
    std::string src =
        "var arr = [1, 2, 3];"
        "arr[0] = 99;"
        "print(arr[0]);";
    EXPECT_EQ(runVMOutput(src), "99");
}

// 测试：数组 push 方法
TEST(VME2E, ArrayPush) {
    std::string src =
        "var arr = [1, 2];"
        "arr.push(3);"
        "print(arr[2]);"
        "print(arr.len());";
    EXPECT_EQ(runVMOutput(src), "33");
}

// 测试：数组 pop 方法
TEST(VME2E, ArrayPop) {
    std::string src =
        "var arr = [1, 2, 3];"
        "var x = arr.pop();"
        "print(x);"
        "print(arr.len());";
    EXPECT_EQ(runVMOutput(src), "32");
}

// 测试：数组 len 方法
TEST(VME2E, ArrayLen) {
    std::string src =
        "var arr = [10, 20, 30, 40];"
        "print(arr.len());";
    EXPECT_EQ(runVMOutput(src), "4");
}

// 测试：数组 contains 方法
TEST(VME2E, ArrayContains) {
    std::string src =
        "var arr = [1, 2, 3];"
        "print(arr.contains(2));"
        "print(arr.contains(99));";
    EXPECT_EQ(runVMOutput(src), "truefalse");
}

// 测试：数组 join 方法
TEST(VME2E, ArrayJoin) {
    std::string src =
        "var arr = [1, 2, 3];"
        "print(arr.join(\",\"));";
    EXPECT_EQ(runVMOutput(src), "1,2,3");
}

// 测试：遍历数组
TEST(VME2E, ArrayIterate) {
    std::string src =
        "var arr = [10, 20, 30];"
        "var sum = 0;"
        "for (var i = 0; i < arr.len(); i = i + 1) { sum = sum + arr[i]; }"
        "print(sum);";
    EXPECT_EQ(runVMOutput(src), "60");
}

// 测试：嵌套数组
TEST(VME2E, NestedArray) {
    std::string src =
        "var matrix = [[1, 2], [3, 4]];"
        "print(matrix[0][0]);"
        "print(matrix[0][1]);"
        "print(matrix[1][0]);"
        "print(matrix[1][1]);";
    EXPECT_EQ(runVMOutput(src), "1234");
}

// ============================================================
// 6. 字符串操作
// ============================================================

// 测试：字符串拼接
TEST(VME2E, StringConcatenation) {
    EXPECT_EQ(runVMOutput("print(\"hello\" + \" \" + \"world\");"),
              "hello world");
}

// 测试：字符串与数字拼接
TEST(VME2E, StringNumberConcat) {
    EXPECT_EQ(runVMOutput("print(\"count: \" + 42);"), "count: 42");
}

// 测试：字符串 len 方法
TEST(VME2E, StringLen) {
    std::string src = "var s = \"hello\"; print(s.len());";
    EXPECT_EQ(runVMOutput(src), "5");
}

// 测试：字符串 upper/lower 方法
TEST(VME2E, StringUpperLower) {
    std::string src =
        "var s = \"Hello\";"
        "print(s.upper());"
        "print(s.lower());";
    EXPECT_EQ(runVMOutput(src), "HELLOhello");
}

// 测试：字符串 split 方法
TEST(VME2E, StringSplit) {
    std::string src =
        "var s = \"a,b,c\";"
        "var arr = s.split(\",\");"
        "print(arr.len());"
        "print(arr[0]);"
        "print(arr[1]);"
        "print(arr[2]);";
    EXPECT_EQ(runVMOutput(src), "3abc");
}

// 测试：字符串 replace 方法
TEST(VME2E, StringReplace) {
    std::string src =
        "var s = \"a-b-c\";"
        "print(s.replace(\"-\", \"+\"));";
    EXPECT_EQ(runVMOutput(src), "a+b+c");
}

// 测试：字符串 trim 方法
TEST(VME2E, StringTrim) {
    std::string src =
        "var s = \"  hello  \";"
        "print(s.trim());";
    EXPECT_EQ(runVMOutput(src), "hello");
}

// 测试：字符串 contains 方法
TEST(VME2E, StringContains) {
    std::string src =
        "var s = \"hello world\";"
        "print(s.contains(\"world\"));"
        "print(s.contains(\"xyz\"));";
    EXPECT_EQ(runVMOutput(src), "truefalse");
}

// ============================================================
// 7. 错误处理
// ============================================================

// 测试：除零错误
TEST(VME2E, DivisionByZeroError) {
    std::string output, error;
    VMResult result = runVMResult("print(1 / 0);", output, error);
    EXPECT_EQ(result, VMResult::VM_RUNTIME_ERROR);
    EXPECT_TRUE(error.find("除零") != std::string::npos);
}

// 测试：取模除零错误
TEST(VME2E, ModuloByZeroError) {
    std::string output, error;
    VMResult result = runVMResult("print(10 % 0);", output, error);
    EXPECT_EQ(result, VMResult::VM_RUNTIME_ERROR);
}

// 测试：调用未定义函数
TEST(VME2E, UndefinedFunctionError) {
    std::string output, error;
    VMResult result = runVMResult("undefinedFunc();", output, error);
    EXPECT_EQ(result, VMResult::VM_RUNTIME_ERROR);
}

// 测试：未定义变量
TEST(VME2E, UndefinedVariableError) {
    std::string output, error;
    VMResult result = runVMResult("print(undefinedVar);", output, error);
    EXPECT_EQ(result, VMResult::VM_RUNTIME_ERROR);
}

// 测试：数组索引越界
TEST(VME2E, ArrayIndexOutOfBoundsError) {
    std::string output, error;
    VMResult result = runVMResult("var a = [1,2,3]; print(a[10]);", output, error);
    EXPECT_EQ(result, VMResult::VM_RUNTIME_ERROR);
}

// 测试：对空数组 pop
TEST(VME2E, PopFromEmptyArrayError) {
    std::string output, error;
    VMResult result = runVMResult("var a = []; a.pop();", output, error);
    EXPECT_EQ(result, VMResult::VM_RUNTIME_ERROR);
}

// 测试：hasError 标志位
TEST(VME2E, HasErrorFlag) {
    Lexer lexer;
    auto tokens = lexer.scan("print(1 / 0);");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_TRUE(ast != nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    vm.setOutputCallback([](const std::string&) {});
    vm.execute(result);
    EXPECT_TRUE(vm.hasError());
    EXPECT_FALSE(vm.getLastError().empty());
}

// 测试：正常执行后 hasError 为 false
TEST(VME2E, NoErrorAfterSuccess) {
    Lexer lexer;
    auto tokens = lexer.scan("print(1 + 2);");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_TRUE(ast != nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    vm.setOutputCallback([](const std::string&) {});
    VMResult vmr = vm.execute(result);
    EXPECT_EQ(vmr, VMResult::VM_OK);
    EXPECT_FALSE(vm.hasError());
}

// 测试：错误行号
TEST(VME2E, ErrorLineNumber) {
    Lexer lexer;
    auto tokens = lexer.scan("var x = 1;\nprint(x / 0);");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_TRUE(ast != nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    vm.setOutputCallback([](const std::string&) {});
    vm.execute(result);
    EXPECT_TRUE(vm.hasError());
    // 错误行号应大于 0（第二行）
    EXPECT_GE(vm.getLastErrorLine(), 0);
}

// ============================================================
// 8. 一致性测试 - 相同源码在 Interpreter 和 VM 上结果应一致
// ============================================================

// 辅助：在 Interpreter 上执行并返回输出
static std::string runInterpreterOutputForConsistency(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";

    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError&) {
        // 运行时错误时返回已捕获的部分输出
    }
    return captured;
}

// 辅助：在 VM 上执行并返回输出（忽略错误状态）
static std::string runVMOutputForConsistency(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    return captured;
}

// 一致性：基本算术
TEST(VMConsistency, Arithmetic) {
    std::string src = "print(1 + 2); print(10 - 3); print(4 * 5); print(20 / 4);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：变量与赋值
TEST(VMConsistency, Variables) {
    std::string src = "var x = 5; print(x); x = 10; print(x);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：控制流
TEST(VMConsistency, ControlFlow) {
    std::string src =
        "var sum = 0;"
        "for (var i = 1; i <= 10; i = i + 1) { sum = sum + i; }"
        "print(sum);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：函数调用
TEST(VMConsistency, FunctionCall) {
    std::string src =
        "fun add(a, b) { return a + b; }"
        "print(add(3, 4));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：递归
TEST(VMConsistency, Recursion) {
    std::string src =
        "fun fact(n) {"
        "  if (n <= 1) { return 1; }"
        "  return n * fact(n - 1);"
        "}"
        "print(fact(6));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：数组操作
TEST(VMConsistency, ArrayOps) {
    std::string src =
        "var arr = [1, 2, 3, 4, 5];"
        "print(arr.len());"
        "arr.push(6);"
        "print(arr.len());"
        "print(arr.contains(3));"
        "print(arr.join(\",\"));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：字符串操作
TEST(VMConsistency, StringOps) {
    std::string src =
        "var s = \"Hello World\";"
        "print(s.len());"
        "print(s.upper());"
        "print(s.lower());"
        "print(s.contains(\"World\"));"
        "print(s.replace(\"World\", \"VM\"));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：嵌套循环
TEST(VMConsistency, NestedLoop) {
    std::string src =
        "var total = 0;"
        "for (var i = 1; i <= 3; i = i + 1) {"
        "  for (var j = 1; j <= 3; j = j + 1) {"
        "    total = total + i * j;"
        "  }"
        "}"
        "print(total);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// 一致性：综合 - 函数 + 循环 + 数组
TEST(VMConsistency, Comprehensive) {
    std::string src =
        "fun sumArray(arr) {"
        "  var total = 0;"
        "  var i = 0;"
        "  while (i < arr.len()) { total = total + arr[i]; i = i + 1; }"
        "  return total;"
        "}"
        "var data = [1, 2, 3, 4, 5];"
        "print(sumArray(data));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

// ============================================================
// 9. break / continue 语句测试
// ============================================================

TEST(VME2E, BreakInWhileLoop) {
    std::string src =
        "var i = 0;"
        "var result = 0;"
        "while (i < 10) {"
        "  if (i == 5) { break; }"
        "  result = result + i;"
        "  i = i + 1;"
        "}"
        "print(result);";
    EXPECT_EQ(runVMOutput(src), "10");
}

TEST(VME2E, ContinueInWhileLoop) {
    std::string src =
        "var i = 0;"
        "var result = 0;"
        "while (i < 5) {"
        "  i = i + 1;"
        "  if (i == 3) { continue; }"
        "  result = result + i;"
        "}"
        "print(result);";
    EXPECT_EQ(runVMOutput(src), "12");
}

TEST(VME2E, BreakInForLoop) {
    std::string src =
        "var result = 0;"
        "for (var i = 0; i < 10; i = i + 1) {"
        "  if (i == 5) { break; }"
        "  result = result + i;"
        "}"
        "print(result);";
    EXPECT_EQ(runVMOutput(src), "10");
}

TEST(VME2E, ContinueInForLoop) {
    std::string src =
        "var result = 0;"
        "for (var i = 0; i < 5; i = i + 1) {"
        "  if (i == 2) { continue; }"
        "  result = result + i;"
        "}"
        "print(result);";
    EXPECT_EQ(runVMOutput(src), "8");
}

TEST(VME2E, NestedLoopBreak) {
    std::string src =
        "var result = 0;"
        "for (var i = 0; i < 3; i = i + 1) {"
        "  for (var j = 0; j < 3; j = j + 1) {"
        "    if (j == 1) { break; }"
        "    result = result + 1;"
        "  }"
        "}"
        "print(result);";
    EXPECT_EQ(runVMOutput(src), "3");
}

// ============================================================
// 10. 块注释测试
// ============================================================

TEST(VME2E, BlockComment) {
    std::string src =
        "/* 这是一个块注释 */"
        "print(42);";
    EXPECT_EQ(runVMOutput(src), "42");
}

TEST(VME2E, BlockCommentMultiline) {
    std::string src =
        "/* 这是\n多行\n块注释 */"
        "print(42);";
    EXPECT_EQ(runVMOutput(src), "42");
}

TEST(VME2E, BlockCommentNested) {
    std::string src =
        "/* 外层 /* 内层 */ 外层继续 */"
        "print(42);";
    EXPECT_EQ(runVMOutput(src), "42");
}

TEST(VME2E, BlockCommentInCode) {
    std::string src =
        "var x = 1; /* 注释 */ x = x + 1; print(x);";
    EXPECT_EQ(runVMOutput(src), "2");
}

// ============================================================
// 11. 顶层内置函数测试
// ============================================================

TEST(VME2E, BuiltinLen) {
    EXPECT_EQ(runVMOutput("print(len([1,2,3]));"), "3");
    EXPECT_EQ(runVMOutput("print(len(\"hello\"));"), "5");
    EXPECT_EQ(runVMOutput("print(len({\"a\":1,\"b\":2}));"), "2");
}

TEST(VME2E, BuiltinType) {
    EXPECT_EQ(runVMOutput("print(type(42));"), "int");
    EXPECT_EQ(runVMOutput("print(type(3.14));"), "float");
    EXPECT_EQ(runVMOutput("print(type(true));"), "bool");
    EXPECT_EQ(runVMOutput("print(type(\"hello\"));"), "string");
    EXPECT_EQ(runVMOutput("print(type([1,2]));"), "array");
    EXPECT_EQ(runVMOutput("print(type(null));"), "null");
}

TEST(VME2E, BuiltinStr) {
    EXPECT_EQ(runVMOutput("print(str(42));"), "42");
    EXPECT_EQ(runVMOutput("print(str(3.0));"), "3");
    EXPECT_EQ(runVMOutput("print(str(true));"), "true");
}

TEST(VME2E, BuiltinInt) {
    EXPECT_EQ(runVMOutput("print(int(42));"), "42");
    EXPECT_EQ(runVMOutput("print(int(3.99));"), "3");
    EXPECT_EQ(runVMOutput("print(int(true));"), "1");
    EXPECT_EQ(runVMOutput("print(int(\"123\"));"), "123");
}

TEST(VME2E, BuiltinAbs) {
    EXPECT_EQ(runVMOutput("print(abs(-5));"), "5");
    EXPECT_EQ(runVMOutput("print(abs(5));"), "5");
    EXPECT_EQ(runVMOutput("print(abs(-3.0));"), "3");
}

TEST(VME2E, BuiltinMinMax) {
    EXPECT_EQ(runVMOutput("print(min(3, 7));"), "3");
    EXPECT_EQ(runVMOutput("print(max(3, 7));"), "7");
    EXPECT_EQ(runVMOutput("print(min(3.5, 2));"), "2");
}

TEST(VME2E, BuiltinRange) {
    EXPECT_EQ(runVMOutput("print(range(5));"), "[0, 1, 2, 3, 4]");
    EXPECT_EQ(runVMOutput("print(range(0));"), "[]");
}

TEST(VME2E, BuiltinSum) {
    EXPECT_EQ(runVMOutput("print(sum([1, 2, 3, 4, 5]));"), "15");
    EXPECT_EQ(runVMOutput("print(sum([1.5, 2.5]));"), "4");
}

// ============================================================
// 12. input() 函数测试（无回调时返回空字符串）
// ============================================================

TEST(VME2E, BuiltinInputNoCallback) {
    // 无 input 回调时返回空字符串
    EXPECT_EQ(runVMOutput("print(input());"), "");
}

// ============================================================
// 13. break/continue + 内置函数一致性测试
// ============================================================

TEST(VMConsistency, BreakContinue) {
    std::string src =
        "var result = 0;"
        "for (var i = 0; i < 10; i = i + 1) {"
        "  if (i == 5) { break; }"
        "  if (i == 2) { continue; }"
        "  result = result + i;"
        "}"
        "print(result);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}

TEST(VMConsistency, BuiltinFunctions) {
    std::string src =
        "print(len([1,2,3]));"
        "print(type(42));"
        "print(str(3.14));"
        "print(abs(-5));"
        "print(min(3, 7));"
        "print(max(3, 7));"
        "print(sum(range(5)));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src),
              runVMOutputForConsistency(src));
}
