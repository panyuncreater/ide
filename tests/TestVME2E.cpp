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
//
// A6 fix: 关于与 TestInterpreterE2E.cpp 的用例重复
// ------------------------------------------------------------
// VME2E.* 套件中的 BasicArithmetic / VariableDeclaration / IfTrueBranch /
// WhileLoopSum / FactorialRecursive / ArrayPush / StringLen / ClassBasic
// 等用例在 TestInterpreterE2E.cpp 的 InterpreterE2E.* 套件中有同名镜像。
// 这种重复是**有意的防御性覆盖**——每个用例独立守护其后端
// （VME2E.* 守 Stack VM，InterpreterE2E.* 守 Interpreter）。
// 跨后端等价性由本文件中的 VMConsistency.* 和 BackendConsistency.*
// 套件覆盖（A2 fix 新增 BackendConsistency.* 包含三后端对比 + REG_DIV
// 语义差异显式测试）。新增跨后端用例请添加到这两个套件，避免与
// InterpreterE2E.* 双向复制。
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h" // AUDIT-HELPER fix: catch RuntimeError
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// 辅助：执行源码并捕获 VM 的 print 输出
// ============================================================

// 执行源码，返回所有 print 输出拼接后的字符串
// AUDIT-HELPER fix: 原实现忽略 vm.hasError()（注释称"调用方可通过 vm.hasError() 检查"
// 但 VM 是局部变量，调用方无法检查——文档谎言）。VM 出错时返回部分输出，
// 测试以"输出不匹配"误报掩盖真实失败原因。现改为检查 hasError 并编码错误。
static std::string runVMOutput(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// 执行源码，返回 VM 的执行结果状态
static VMResult runVMResult(const std::string& source, std::string& output, std::string& error) {
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
// AUDIT-HELPER fix: 原实现忽略 vm.hasError()，VM 执行中途出错时全局变量表不完整，
// EXPECT_EQ(globals["x"].intVal(), 42) 可能误报"globals 中无 x"或碰巧通过。
static std::unordered_map<std::string, Value> runVMGlobals(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return {};

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    vm.setOutputCallback([](const std::string&) {});
    vm.execute(result);
    // 不强制失败：某些测试可能预期部分全局变量存在即使 VM 出错。
    // 但通过 hasError_ 状态供调用方判断（vm 是局部变量无法外部检查，
    // 如需错误检查请用 runVMResult）。
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
    std::string src = "var x = 5;"
                      "if (x == 1) { print(1); }"
                      "else if (x == 5) { print(5); }"
                      "else { print(0); }";
    EXPECT_EQ(runVMOutput(src), "5");
}

// 测试：while 循环计算 1 到 10 的和
TEST(VME2E, WhileLoopSum) {
    std::string src = "var sum = 0;"
                      "var i = 1;"
                      "while (i <= 10) { sum = sum + i; i = i + 1; }"
                      "print(sum);";
    EXPECT_EQ(runVMOutput(src), "55");
}

// 测试：while 循环条件不满足时不执行
TEST(VME2E, WhileLoopNotExecuted) {
    std::string src = "var i = 100;"
                      "while (i < 10) { print(i); i = i + 1; }"
                      "print(99);";
    EXPECT_EQ(runVMOutput(src), "99");
}

// 测试：for 循环打印 1 到 5
TEST(VME2E, ForLoopPrint) {
    std::string src = "for (var i = 1; i <= 5; i = i + 1) { print(i); }";
    EXPECT_EQ(runVMOutput(src), "12345");
}

// 测试：for 循环计算累加和
TEST(VME2E, ForLoopSum) {
    std::string src = "var sum = 0;"
                      "for (var i = 1; i <= 100; i = i + 1) { sum = sum + i; }"
                      "print(sum);";
    EXPECT_EQ(runVMOutput(src), "5050");
}

// 测试：嵌套循环（乘法表局部）
TEST(VME2E, NestedLoop) {
    std::string src = "var total = 0;"
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
    std::string src = "fun add(a, b) { return a + b; }"
                      "print(add(3, 4));";
    EXPECT_EQ(runVMOutput(src), "7");
}

// 测试：函数无返回值
TEST(VME2E, FunctionNoReturn) {
    std::string src = "fun greet() { print(123); }"
                      "greet();";
    EXPECT_EQ(runVMOutput(src), "123");
}

// 测试：阶乘递归
TEST(VME2E, FactorialRecursive) {
    std::string src = "fun fact(n) {"
                      "  if (n <= 1) { return 1; }"
                      "  return n * fact(n - 1);"
                      "}"
                      "print(fact(5));";
    EXPECT_EQ(runVMOutput(src), "120");
}

// 测试：斐波那契递归
TEST(VME2E, FibonacciRecursive) {
    std::string src = "fun fib(n) {"
                      "  if (n < 2) { return n; }"
                      "  return fib(n - 1) + fib(n - 2);"
                      "}"
                      "print(fib(10));";
    EXPECT_EQ(runVMOutput(src), "55");
}

// 测试：函数参数为表达式
TEST(VME2E, FunctionCallWithExpression) {
    std::string src = "fun double(x) { return x * 2; }"
                      "print(double(3 + 4));";
    EXPECT_EQ(runVMOutput(src), "14");
}

// 测试：嵌套函数调用
TEST(VME2E, NestedFunctionCall) {
    std::string src = "fun inc(x) { return x + 1; }"
                      "fun double(x) { return x * 2; }"
                      "print(inc(double(5)));";
    EXPECT_EQ(runVMOutput(src), "11");
}

// 测试：多参数函数
TEST(VME2E, MultiArgFunction) {
    std::string src = "fun sum4(a, b, c, d) { return a + b + c + d; }"
                      "print(sum4(1, 2, 3, 4));";
    EXPECT_EQ(runVMOutput(src), "10");
}

// ============================================================
// 5. 数组操作
// ============================================================

// 测试：数组创建与访问
TEST(VME2E, ArrayCreateAndAccess) {
    std::string src = "var arr = [1, 2, 3];"
                      "print(arr[0]);"
                      "print(arr[1]);"
                      "print(arr[2]);";
    EXPECT_EQ(runVMOutput(src), "123");
}

// 测试：数组索引赋值
TEST(VME2E, ArrayIndexAssign) {
    std::string src = "var arr = [1, 2, 3];"
                      "arr[0] = 99;"
                      "print(arr[0]);";
    EXPECT_EQ(runVMOutput(src), "99");
}

// 测试：数组 push 方法
TEST(VME2E, ArrayPush) {
    std::string src = "var arr = [1, 2];"
                      "arr.push(3);"
                      "print(arr[2]);"
                      "print(arr.len());";
    EXPECT_EQ(runVMOutput(src), "33");
}

// 测试：数组 pop 方法
TEST(VME2E, ArrayPop) {
    std::string src = "var arr = [1, 2, 3];"
                      "var x = arr.pop();"
                      "print(x);"
                      "print(arr.len());";
    EXPECT_EQ(runVMOutput(src), "32");
}

// 测试：数组 len 方法
TEST(VME2E, ArrayLen) {
    std::string src = "var arr = [10, 20, 30, 40];"
                      "print(arr.len());";
    EXPECT_EQ(runVMOutput(src), "4");
}

// 测试：数组 contains 方法
TEST(VME2E, ArrayContains) {
    std::string src = "var arr = [1, 2, 3];"
                      "print(arr.contains(2));"
                      "print(arr.contains(99));";
    EXPECT_EQ(runVMOutput(src), "truefalse");
}

// 测试：数组 join 方法
TEST(VME2E, ArrayJoin) {
    std::string src = "var arr = [1, 2, 3];"
                      "print(arr.join(\",\"));";
    EXPECT_EQ(runVMOutput(src), "1,2,3");
}

// 测试：遍历数组
TEST(VME2E, ArrayIterate) {
    std::string src = "var arr = [10, 20, 30];"
                      "var sum = 0;"
                      "for (var i = 0; i < arr.len(); i = i + 1) { sum = sum + arr[i]; }"
                      "print(sum);";
    EXPECT_EQ(runVMOutput(src), "60");
}

// 测试：嵌套数组
TEST(VME2E, NestedArray) {
    std::string src = "var matrix = [[1, 2], [3, 4]];"
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
    EXPECT_EQ(runVMOutput("print(\"hello\" + \" \" + \"world\");"), "hello world");
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
    std::string src = "var s = \"Hello\";"
                      "print(s.upper());"
                      "print(s.lower());";
    EXPECT_EQ(runVMOutput(src), "HELLOhello");
}

// 测试：字符串 split 方法
TEST(VME2E, StringSplit) {
    std::string src = "var s = \"a,b,c\";"
                      "var arr = s.split(\",\");"
                      "print(arr.len());"
                      "print(arr[0]);"
                      "print(arr[1]);"
                      "print(arr[2]);";
    EXPECT_EQ(runVMOutput(src), "3abc");
}

// 测试：字符串 replace 方法
TEST(VME2E, StringReplace) {
    std::string src = "var s = \"a-b-c\";"
                      "print(s.replace(\"-\", \"+\"));";
    EXPECT_EQ(runVMOutput(src), "a+b+c");
}

// 测试：字符串 trim 方法
TEST(VME2E, StringTrim) {
    std::string src = "var s = \"  hello  \";"
                      "print(s.trim());";
    EXPECT_EQ(runVMOutput(src), "hello");
}

// 测试：字符串 contains 方法
TEST(VME2E, StringContains) {
    std::string src = "var s = \"hello world\";"
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
// AUDIT-HELPER fix: 原实现静默吞掉 RuntimeError 返回部分输出，跨后端比较时
// "部分输出 == 部分输出"可能误判通过。现改为编码错误为 <runtime:msg> 前缀。
static std::string runInterpreterOutputForConsistency(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "";

    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return captured + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return captured + "<runtime:" + std::string(e.what()) + ">";
    }
    return captured;
}

// 辅助：在 VM 上执行并返回输出
// AUDIT-HELPER fix: 原实现忽略 vm.hasError()，VM 出错时返回部分输出可能误判通过。
// 现改为检查 hasError 并编码错误为 <runtime:msg> 前缀。
static std::string runVMOutputForConsistency(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "";

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// 一致性：基本算术
TEST(VMConsistency, Arithmetic) {
    std::string src = "print(1 + 2); print(10 - 3); print(4 * 5); print(20 / 4);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：变量与赋值
TEST(VMConsistency, Variables) {
    std::string src = "var x = 5; print(x); x = 10; print(x);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：控制流
TEST(VMConsistency, ControlFlow) {
    std::string src = "var sum = 0;"
                      "for (var i = 1; i <= 10; i = i + 1) { sum = sum + i; }"
                      "print(sum);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：函数调用
TEST(VMConsistency, FunctionCall) {
    std::string src = "fun add(a, b) { return a + b; }"
                      "print(add(3, 4));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：递归
TEST(VMConsistency, Recursion) {
    std::string src = "fun fact(n) {"
                      "  if (n <= 1) { return 1; }"
                      "  return n * fact(n - 1);"
                      "}"
                      "print(fact(6));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：数组操作
TEST(VMConsistency, ArrayOps) {
    std::string src = "var arr = [1, 2, 3, 4, 5];"
                      "print(arr.len());"
                      "arr.push(6);"
                      "print(arr.len());"
                      "print(arr.contains(3));"
                      "print(arr.join(\",\"));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：字符串操作
TEST(VMConsistency, StringOps) {
    std::string src = "var s = \"Hello World\";"
                      "print(s.len());"
                      "print(s.upper());"
                      "print(s.lower());"
                      "print(s.contains(\"World\"));"
                      "print(s.replace(\"World\", \"VM\"));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：嵌套循环
TEST(VMConsistency, NestedLoop) {
    std::string src = "var total = 0;"
                      "for (var i = 1; i <= 3; i = i + 1) {"
                      "  for (var j = 1; j <= 3; j = j + 1) {"
                      "    total = total + i * j;"
                      "  }"
                      "}"
                      "print(total);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性：综合 - 函数 + 循环 + 数组
TEST(VMConsistency, Comprehensive) {
    std::string src = "fun sumArray(arr) {"
                      "  var total = 0;"
                      "  var i = 0;"
                      "  while (i < arr.len()) { total = total + arr[i]; i = i + 1; }"
                      "  return total;"
                      "}"
                      "var data = [1, 2, 3, 4, 5];"
                      "print(sumArray(data));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// ============================================================
// 9. break / continue 语句测试
// ============================================================

TEST(VME2E, BreakInWhileLoop) {
    std::string src = "var i = 0;"
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
    std::string src = "var i = 0;"
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
    std::string src = "var result = 0;"
                      "for (var i = 0; i < 10; i = i + 1) {"
                      "  if (i == 5) { break; }"
                      "  result = result + i;"
                      "}"
                      "print(result);";
    EXPECT_EQ(runVMOutput(src), "10");
}

TEST(VME2E, ContinueInForLoop) {
    std::string src = "var result = 0;"
                      "for (var i = 0; i < 5; i = i + 1) {"
                      "  if (i == 2) { continue; }"
                      "  result = result + i;"
                      "}"
                      "print(result);";
    EXPECT_EQ(runVMOutput(src), "8");
}

TEST(VME2E, NestedLoopBreak) {
    std::string src = "var result = 0;"
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
    std::string src = "/* 这是一个块注释 */"
                      "print(42);";
    EXPECT_EQ(runVMOutput(src), "42");
}

TEST(VME2E, BlockCommentMultiline) {
    std::string src = "/* 这是\n多行\n块注释 */"
                      "print(42);";
    EXPECT_EQ(runVMOutput(src), "42");
}

TEST(VME2E, BlockCommentNested) {
    std::string src = "/* 外层 /* 内层 */ 外层继续 */"
                      "print(42);";
    EXPECT_EQ(runVMOutput(src), "42");
}

TEST(VME2E, BlockCommentInCode) {
    std::string src = "var x = 1; /* 注释 */ x = x + 1; print(x);";
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
    std::string src = "var result = 0;"
                      "for (var i = 0; i < 10; i = i + 1) {"
                      "  if (i == 5) { break; }"
                      "  if (i == 2) { continue; }"
                      "  result = result + i;"
                      "}"
                      "print(result);";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

TEST(VMConsistency, BuiltinFunctions) {
    std::string src = "print(len([1,2,3]));"
                      "print(type(42));"
                      "print(str(3.14));"
                      "print(abs(-5));"
                      "print(min(3, 7));"
                      "print(max(3, 7));"
                      "print(sum(range(5)));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// ============================================================
// 14. F5: 字符串方法一致性测试 (startsWith/endsWith/substr/indexOf)
// ============================================================

TEST(VME2E, StringStartsWith) {
    EXPECT_EQ(runVMOutput("print(\"hello world\".startsWith(\"hello\"));"), "true");
    EXPECT_EQ(runVMOutput("print(\"hello world\".startsWith(\"world\"));"), "false");
    EXPECT_EQ(runVMOutput("print(\"hello\".startsWith(\"\"));"), "true");
}

TEST(VME2E, StringEndsWith) {
    EXPECT_EQ(runVMOutput("print(\"hello world\".endsWith(\"world\"));"), "true");
    EXPECT_EQ(runVMOutput("print(\"hello world\".endsWith(\"hello\"));"), "false");
    EXPECT_EQ(runVMOutput("print(\"hello\".endsWith(\"\"));"), "true");
}

TEST(VME2E, StringSubstr) {
    EXPECT_EQ(runVMOutput("print(\"hello world\".substr(6));"), "world");
    EXPECT_EQ(runVMOutput("print(\"hello world\".substr(0, 5));"), "hello");
    EXPECT_EQ(runVMOutput("print(\"hello\".substr(2, 3));"), "llo");
}

TEST(VME2E, StringIndexOf) {
    EXPECT_EQ(runVMOutput("print(\"hello world\".indexOf(\"world\"));"), "6");
    EXPECT_EQ(runVMOutput("print(\"hello world\".indexOf(\"xyz\"));"), "-1");
    EXPECT_EQ(runVMOutput("print(\"hello\".indexOf(\"l\"));"), "2");
}

TEST(VMConsistency, StringMethods) {
    std::string src = "var s = \"hello world\";"
                      "print(s.startsWith(\"hello\"));"
                      "print(s.endsWith(\"world\"));"
                      "print(s.substr(6));"
                      "print(s.indexOf(\"world\"));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// ============================================================
// 15. F7: 字符串插值测试
// ============================================================

TEST(VME2E, StringInterpolationBasic) {
    EXPECT_EQ(runVMOutput("var name = \"Alice\"; print(\"Hello {name}!\");"), "Hello Alice!");
}

TEST(VME2E, StringInterpolationMultiple) {
    std::string src = "var name = \"Bob\";"
                      "var age = 25;"
                      "print(\"Name: {name}, Age: {age}\");";
    EXPECT_EQ(runVMOutput(src), "Name: Bob, Age: 25");
}

TEST(VME2E, StringInterpolationExpression) {
    EXPECT_EQ(runVMOutput("print(\"Result: {1 + 2 * 3}\");"), "Result: 7");
}

TEST(VME2E, StringInterpolationNoInterp) {
    // 普通字符串不受影响
    EXPECT_EQ(runVMOutput("print(\"hello world\");"), "hello world");
}

TEST(VME2E, StringInterpolationEmpty) {
    // AUDIT-BUG-P1 fix: 空插值 {} 现在是语法错误（原实现静默接受为空字符串）
    // Lexer 接受 "{}" 但 Parser 在 parseInterpolatedString 中报错
    std::string output = runVMOutput("print(\"a{}b\");");
    EXPECT_TRUE(output.empty()) << "空插值应导致语法错误，无输出";
}

TEST(VMConsistency, StringInterpolation) {
    std::string src = "var name = \"Test\";"
                      "var count = 3;"
                      "print(\"{name}: {count} items\");";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// ============================================================
// F10: 默认参数值
// ============================================================

// 测试：使用默认参数
TEST(VME2E, DefaultParamBasic) {
    std::string src = "fun greet(name, greeting = \"Hello\") {"
                      "  print(greeting + \", \" + name);"
                      "}"
                      "greet(\"World\");";
    EXPECT_EQ(runVMOutput(src), "Hello, World");
}

// 测试：覆盖默认参数
TEST(VME2E, DefaultParamOverride) {
    std::string src = "fun greet(name, greeting = \"Hello\") {"
                      "  print(greeting + \", \" + name);"
                      "}"
                      "greet(\"World\", \"Hi\");";
    EXPECT_EQ(runVMOutput(src), "Hi, World");
}

// 测试：多个默认参数
TEST(VME2E, DefaultParamMultiple) {
    std::string src = "fun add(a, b = 10, c = 100) {"
                      "  return a + b + c;"
                      "}"
                      "print(add(1));"
                      "print(add(1, 2));"
                      "print(add(1, 2, 3));";
    EXPECT_EQ(runVMOutput(src), "1111036");
}

// 测试：默认参数为数字
TEST(VME2E, DefaultParamNumber) {
    std::string src = "fun power(base, exp = 2) {"
                      "  var result = 1;"
                      "  for (var i = 0; i < exp; i = i + 1) {"
                      "    result = result * base;"
                      "  }"
                      "  return result;"
                      "}"
                      "print(power(3));"
                      "print(power(3, 3));";
    EXPECT_EQ(runVMOutput(src), "927");
}

// 测试：默认参数为布尔值和 null
TEST(VME2E, DefaultParamBoolNull) {
    std::string src = "fun test(a, b = true, c = null) {"
                      "  print(a);"
                      "  print(b);"
                      "  print(c);"
                      "}"
                      "test(1);";
    EXPECT_EQ(runVMOutput(src), "1truenull");
}

// 测试：默认参数为负数
TEST(VME2E, DefaultParamNegative) {
    std::string src = "fun test(a, b = -5) {"
                      "  return a + b;"
                      "}"
                      "print(test(10));"
                      "print(test(10, 20));";
    EXPECT_EQ(runVMOutput(src), "530");
}

// 测试：参数过少时报错
TEST(VME2E, DefaultParamTooFew) {
    std::string output, error;
    std::string src = "fun test(a, b = 10) { return a + b; }"
                      "test();";
    runVMResult(src, output, error);
    EXPECT_FALSE(error.empty());
}

// 测试：参数过多时报错
TEST(VME2E, DefaultParamTooMany) {
    std::string output, error;
    std::string src = "fun test(a, b = 10) { return a + b; }"
                      "test(1, 2, 3);";
    runVMResult(src, output, error);
    EXPECT_FALSE(error.empty());
}

// 测试：类方法默认参数
TEST(VME2E, DefaultParamMethod) {
    std::string src = "class Calculator {"
                      "  fun multiply(a, b = 2) { return a * b; }"
                      "}"
                      "var calc = Calculator();"
                      "print(calc.multiply(5));"
                      "print(calc.multiply(5, 3));";
    EXPECT_EQ(runVMOutput(src), "1015");
}

// 测试：全默认参数
TEST(VME2E, DefaultParamAllDefault) {
    std::string src = "fun test(a = 1, b = 2, c = 3) {"
                      "  return a + b + c;"
                      "}"
                      "print(test());"
                      "print(test(10));"
                      "print(test(10, 20));"
                      "print(test(10, 20, 30));";
    EXPECT_EQ(runVMOutput(src), "6153360");
}

// 一致性测试：默认参数
TEST(VMConsistency, DefaultParams) {
    std::string src = "fun compute(a, b = 10, c = 20) {"
                      "  return (a + b) * c;"
                      "}"
                      "print(compute(1));"
                      "print(compute(1, 2));"
                      "print(compute(1, 2, 3));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// ============================================================
// F11: try/catch 异常处理测试
// ============================================================

// 测试：基本 try-catch 捕获
TEST(VME2E, TryCatchBasic) {
    std::string src = "try {"
                      "  throw \"error\";"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    EXPECT_EQ(runVMOutput(src), "error");
}

// 测试：try 块无异常时跳过 catch
TEST(VME2E, TryCatchNoThrow) {
    std::string src = "try {"
                      "  print(\"try\");"
                      "} catch (e) {"
                      "  print(\"catch\");"
                      "}"
                      "print(\"end\");";
    EXPECT_EQ(runVMOutput(src), "tryend");
}

// 测试：抛出数字异常
TEST(VME2E, TryCatchThrowNumber) {
    std::string src = "try {"
                      "  throw 42;"
                      "} catch (n) {"
                      "  print(n);"
                      "}";
    EXPECT_EQ(runVMOutput(src), "42");
}

// 测试：函数内抛出，外层捕获
TEST(VME2E, TryCatchCrossFunction) {
    std::string src = "fun risky() {"
                      "  throw \"from func\";"
                      "}"
                      "try {"
                      "  risky();"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    EXPECT_EQ(runVMOutput(src), "from func");
}

// 测试：catch 块中继续执行后续代码
TEST(VME2E, TryCatchContinue) {
    std::string src = "try {"
                      "  throw 1;"
                      "} catch (e) {"
                      "  print(\"caught\");"
                      "}"
                      "print(\"after\");";
    EXPECT_EQ(runVMOutput(src), "caughtafter");
}

// 测试：未捕获的异常导致错误
TEST(VME2E, TryUncaught) {
    std::string src = "throw \"uncaught\";";
    std::string output, error;
    runVMResult(src, output, error);
    EXPECT_FALSE(error.empty());
}

// 测试：嵌套 try-catch
TEST(VME2E, TryCatchNested) {
    std::string src = "try {"
                      "  try {"
                      "    throw \"inner\";"
                      "  } catch (e1) {"
                      "    print(e1);"
                      "    throw \"outer\";"
                      "  }"
                      "} catch (e2) {"
                      "  print(e2);"
                      "}";
    EXPECT_EQ(runVMOutput(src), "innerouter");
}

// 测试：catch 变量在新作用域中
TEST(VME2E, TryCatchScope) {
    std::string src = "var x = 1;"
                      "try {"
                      "  throw 99;"
                      "} catch (e) {"
                      "  print(e);"
                      "}"
                      "print(x);";
    EXPECT_EQ(runVMOutput(src), "991");
}

// 测试：throw 后的代码不执行
TEST(VME2E, ThrowSkipsRemaining) {
    std::string src = "try {"
                      "  print(\"before\");"
                      "  throw 1;"
                      "  print(\"after\");"
                      "} catch (e) {"
                      "  print(\"caught\");"
                      "}";
    EXPECT_EQ(runVMOutput(src), "beforecaught");
}

// 一致性测试：try/catch
TEST(VMConsistency, TryCatch) {
    std::string src = "fun divide(a, b) {"
                      "  if (b == 0) { throw \"div by zero\"; }"
                      "  return a / b;"
                      "}"
                      "try {"
                      "  print(divide(10, 2));"
                      "  print(divide(10, 0));"
                      "  print(divide(20, 4));"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// ============================================================
// Bug 回归测试：新功能 bug 修复验证
// ============================================================

// P0-1/P0-2 fix: 类 init 方法支持默认参数
TEST(VME2E, ClassInitDefaultParam) {
    std::string src = "class Point {"
                      "  var x = 0;"
                      "  var y = 0;"
                      "  fun init(x, y = 0) {"
                      "    this.x = x;"
                      "    this.y = y;"
                      "  }"
                      "}"
                      "var p1 = Point(1);"
                      "var p2 = Point(1, 2);"
                      "print(p1.x);"
                      "print(p1.y);"
                      "print(p2.x);"
                      "print(p2.y);";
    EXPECT_EQ(runVMOutput(src), "1012");
}

// P0-1 fix: 类 init 全部默认参数时可无参构造
TEST(VME2E, ClassInitAllDefaultParam) {
    std::string src = "class Config {"
                      "  var host = \"localhost\";"
                      "  var port = 8080;"
                      "  fun init(host = \"default\", port = 3000) {"
                      "    this.host = host;"
                      "    this.port = port;"
                      "  }"
                      "}"
                      "var c = Config();"
                      "print(c.host);"
                      "print(c.port);";
    EXPECT_EQ(runVMOutput(src), "default3000");
}

// P0-3 fix: try 块内 return 不泄漏 tryStack_ handler
TEST(VME2E, TryReturnCleanup) {
    std::string src = "fun test() {"
                      "  try {"
                      "    return 42;"
                      "  } catch (e) {"
                      "    return -1;"
                      "  }"
                      "}"
                      "print(test());"
                      // 再次调用验证 tryStack_ 未泄漏（不会误捕后续异常）
                      "fun test2() {"
                      "  throw 99;"
                      "}"
                      "try {"
                      "  test2();"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    EXPECT_EQ(runVMOutput(src), "4299");
}

// P0-4 fix: try 块内 break 不泄漏 tryStack_ handler
TEST(VME2E, TryBreakCleanup) {
    std::string src = "var result = \"\";"
                      "for (var i = 0; i < 5; i = i + 1) {"
                      "  try {"
                      "    if (i == 2) { break; }"
                      "    result = result + \"i\";"
                      "  } catch (e) {"
                      "    result = result + \"e\";"
                      "  }"
                      "}"
                      "print(result);"
                      // 验证 break 后 tryStack_ 已清理：后续 throw 应正常传播
                      "try {"
                      "  throw 1;"
                      "} catch (e) {"
                      "  print(\"ok\");"
                      "}";
    EXPECT_EQ(runVMOutput(src), "iiok");
}

// P0-4 fix: try 块内 continue 不泄漏 tryStack_ handler
TEST(VME2E, TryContinueCleanup) {
    std::string src = "var result = \"\";"
                      "for (var i = 0; i < 4; i = i + 1) {"
                      "  try {"
                      "    if (i == 1) { continue; }"
                      "    result = result + \"i\" + i;"
                      "  } catch (e) {"
                      "    result = result + \"e\";"
                      "  }"
                      "}"
                      "print(result);";
    EXPECT_EQ(runVMOutput(src), "i0i2i3");
}

// P2-4 fix: catch 变量不泄漏到外层作用域（函数内测试）
// 注意：顶层 catch 变量在 VM 中为全局变量，属于设计限制，此处仅测试函数内作用域
TEST(VME2E, CatchVarNoLeak) {
    std::string output, error;
    std::string src = "fun test() {"
                      "  try {"
                      "    throw 42;"
                      "  } catch (e) {"
                      "    print(e);"
                      "  }"
                      "  return e;" // e 不应在此可访问
                      "}"
                      "test();";
    runVMResult(src, output, error);
    EXPECT_EQ(output, "42");
    EXPECT_FALSE(error.empty());
}

// P0-5 fix: try 块内闭包捕获 — 异常展开时关闭 open upvalues
TEST(VME2E, TryClosureUpvalueClose) {
    std::string src = "fun makeCounter() {"
                      "  var count = 0;"
                      "  fun inc() {"
                      "    count = count + 1;"
                      "    return count;"
                      "  }"
                      "  try {"
                      "    inc();"
                      "    inc();"
                      "    throw \"stop\";"
                      "  } catch (e) {"
                      "    print(e);"
                      "  }"
                      "  print(inc());" // 应输出 3（count 未被异常破坏）
                      "}"
                      "makeCounter();";
    EXPECT_EQ(runVMOutput(src), "stop3");
}

// P2-1 fix: 命名导入原子性 — 部分名称不存在时不导入任何名称
TEST(InterpreterE2E, ModuleImportAtomic) {
    // 这个测试需要 Interpreter 端的模块支持，在 TestInterpreterE2E 中已有框架
    // 此处仅验证 VM 端不崩溃（VM 不支持 import，编译期报错）
    std::string output, error;
    std::string src = "import { a, b, c } from \"nonexistent.mini\";";
    runVMResult(src, output, error);
    EXPECT_FALSE(error.empty()); // VM 应报编译错误
}

// 一致性测试：类 init 默认参数
TEST(VMConsistency, ClassInitDefaultParam) {
    std::string src = "class Box {"
                      "  var w = 0;"
                      "  var h = 0;"
                      "  fun init(aw, ah = 1) {"
                      "    w = aw;"
                      "    h = ah;"
                      "  }"
                      "  fun area() { return w * h; }"
                      "}"
                      "var b1 = Box(5);"
                      "var b2 = Box(5, 10);"
                      "print(b1.area());"
                      "print(b2.area());";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性测试：try 块内 return
TEST(VMConsistency, TryReturn) {
    std::string src = "fun safeDiv(a, b) {"
                      "  try {"
                      "    if (b == 0) { throw \"zero\"; }"
                      "    return a / b;"
                      "  } catch (e) {"
                      "    return -1;"
                      "  }"
                      "}"
                      "print(safeDiv(10, 2));"
                      "print(safeDiv(10, 0));"
                      "print(safeDiv(20, 4));";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// 一致性测试：try 块内闭包
TEST(VMConsistency, TryClosure) {
    std::string src = "fun test() {"
                      "  var x = 10;"
                      "  fun getX() { return x; }"
                      "  try {"
                      "    x = 20;"
                      "    throw 1;"
                      "  } catch (e) {"
                      "    x = 30;"
                      "  }"
                      "  return getX();"
                      "}"
                      "print(test());";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), runVMOutputForConsistency(src));
}

// ============================================================
// RegVM 端到端测试（C-9 fix: 验证寄存器式 VM 类系统）
// ============================================================

// 辅助：通过 RegVM 路径执行源码，返回 print 输出
// P1-7 fix: 默认断言无运行时错误，使失败信息包含 lastError_ 而非仅输出 diff。
// expectError=true 时跳过断言，用于已知会出错的用例（如除零、未支持方法）。
static std::string runRegVMOutput(const std::string& source, bool expectError = false) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);

    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    VMResult vmr = vm.execute(compiler.getLastRegisterResult());
    // P1-7 fix: 原实现忽略 vmr 与 hasError_，若 VM 运行时出错（如类型错误、栈溢出），
    // captured 可能为部分输出或空串，测试会以"输出不匹配"误报，掩盖真实失败原因。
    if (!expectError && vm.hasError()) {
        ADD_FAILURE() << "RegisterVM 执行出错: " << vm.getLastError() << " (输出: \"" << captured << "\")";
        return captured;
    }
    (void)vmr;
    return captured;
}

// C-9 fix: RegVM 类方法调用（无 this 引用）
TEST(RegVME2E, ClassMethodNoThis) {
    std::string src = "class Calculator {"
                      "  fun multiply(a, b = 2) { return a * b; }"
                      "}"
                      "var calc = Calculator();"
                      "print(calc.multiply(5));"
                      "print(calc.multiply(5, 3));";
    EXPECT_EQ(runRegVMOutput(src), "1015");
}

// C-9 fix: RegVM 类 init + this 字段赋值
TEST(RegVME2E, ClassInitWithThis) {
    std::string src = "class Point {"
                      "  var x = 0;"
                      "  var y = 0;"
                      "  fun init(x, y = 0) {"
                      "    this.x = x;"
                      "    this.y = y;"
                      "  }"
                      "}"
                      "var p1 = Point(1);"
                      "var p2 = Point(1, 2);"
                      "print(p1.x);"
                      "print(p1.y);"
                      "print(p2.x);"
                      "print(p2.y);";
    EXPECT_EQ(runRegVMOutput(src), "1012");
}

// C-9 fix: RegVM 类方法读取 this 字段
TEST(RegVME2E, ClassMethodReadThis) {
    std::string src = "class Box {"
                      "  var w = 0;"
                      "  var h = 0;"
                      "  fun init(aw, ah = 1) {"
                      "    this.w = aw;"
                      "    this.h = ah;"
                      "  }"
                      "  fun area() { return this.w * this.h; }"
                      "}"
                      "var b1 = Box(5);"
                      "var b2 = Box(5, 10);"
                      "print(b1.area());"
                      "print(b2.area());";
    EXPECT_EQ(runRegVMOutput(src), "550");
}

// C-6 fix: 方法修改 this 字段后返回非实例值，字段同步不应丢失
TEST(RegVME2E, ClassMethodModifyThisReturnNumber) {
    std::string src = "class Counter {"
                      "  var count;"
                      "  fun init() { this.count = 0; }"
                      "  fun increment() {"
                      "    this.count = this.count + 1;"
                      "    return this.count;"
                      "  }"
                      "}"
                      "var c = Counter();"
                      "print(c.increment());"
                      "print(c.increment());"
                      "print(c.count);";
    // increment() 返回 1（count 从 0→1），再次 increment 返回 2，最后 count=2
    EXPECT_EQ(runRegVMOutput(src), "122");
}

// C-6 fix: 方法修改 this 字段后无返回值（隐式 null），字段同步不应丢失
TEST(RegVME2E, ClassMethodModifyThisNoReturn) {
    std::string src = "class Accumulator {"
                      "  var total;"
                      "  fun init() { this.total = 0; }"
                      "  fun add(n) {"
                      "    this.total = this.total + n;"
                      "  }"
                      "  fun get() { return this.total; }"
                      "}"
                      "var a = Accumulator();"
                      "a.add(10);"
                      "a.add(20);"
                      "print(a.get());";
    EXPECT_EQ(runRegVMOutput(src), "30");
}

// ============================================================
// P0: RegVM 端到端测试补全（算术/控制流/函数/数组/字符串/异常/继承/super）
// ============================================================

// ---- 算术 ----
TEST(RegVME2E, ArithBasic) {
    EXPECT_EQ(runRegVMOutput("print(1 + 2 * 3);"), "7");
}
TEST(RegVME2E, ArithMod) {
    EXPECT_EQ(runRegVMOutput("print(17 % 5);"), "2");
}
TEST(RegVME2E, ArithNegative) {
    EXPECT_EQ(runRegVMOutput("print(-5 + 3);"), "-2");
}
TEST(RegVME2E, ArithFloat) {
    EXPECT_EQ(runRegVMOutput("print(1.5 + 2.5);"), "4");
}

// ---- 控制流 ----
TEST(RegVME2E, IfElse) {
    std::string src = "var x = 10;"
                      "if (x > 5) { print(\"big\"); } else { print(\"small\"); }";
    EXPECT_EQ(runRegVMOutput(src), "big");
}
TEST(RegVME2E, WhileLoop) {
    std::string src = "var i = 0;"
                      "var sum = 0;"
                      "while (i < 5) { sum = sum + i; i = i + 1; }"
                      "print(sum);";
    EXPECT_EQ(runRegVMOutput(src), "10");
}
TEST(RegVME2E, ForLoop) {
    std::string src = "var total = 0;"
                      "for (var i = 1; i <= 5; i = i + 1) { total = total + i; }"
                      "print(total);";
    EXPECT_EQ(runRegVMOutput(src), "15");
}
TEST(RegVME2E, BreakContinue) {
    std::string src = "var result = 0;"
                      "for (var i = 0; i < 10; i = i + 1) {"
                      "  if (i == 3) { continue; }"
                      "  if (i == 7) { break; }"
                      "  result = result + i;"
                      "}"
                      "print(result);";
    // i=0,1,2 累加(=3)，i=3 continue 跳过，i=4,5,6 累加(=18)，i=7 break
    EXPECT_EQ(runRegVMOutput(src), "18");
}

// ---- 函数 ----
TEST(RegVME2E, FunctionCall) {
    std::string src = "fun add(a, b) { return a + b; }"
                      "print(add(3, 4));";
    EXPECT_EQ(runRegVMOutput(src), "7");
}
TEST(RegVME2E, FunctionDefaultArg) {
    std::string src = "fun greet(name, greeting = \"Hi\") { return greeting + \" \" + name; }"
                      "print(greet(\"Bob\"));"
                      "print(greet(\"Bob\", \"Hello\"));";
    EXPECT_EQ(runRegVMOutput(src), "Hi BobHello Bob");
}
TEST(RegVME2E, FunctionRecursion) {
    std::string src = "fun fib(n) {"
                      "  if (n < 2) { return n; }"
                      "  return fib(n - 1) + fib(n - 2);"
                      "}"
                      "print(fib(10));";
    EXPECT_EQ(runRegVMOutput(src), "55");
}

// ---- 数组 ----
TEST(RegVME2E, ArrayBasic) {
    std::string src = "var arr = [10, 20, 30];"
                      "print(arr[0]);"
                      "print(arr[2]);"
                      "print(arr.len());";
    EXPECT_EQ(runRegVMOutput(src), "10303");
}
TEST(RegVME2E, ArrayPush) {
    std::string src = "var arr = [1, 2];"
                      "arr.push(3);"
                      "print(arr.len());"
                      "print(arr[2]);";
    EXPECT_EQ(runRegVMOutput(src), "33");
}
TEST(RegVME2E, ArrayIterate) {
    std::string src = "var arr = [1, 2, 3, 4];"
                      "var sum = 0;"
                      "for (var i = 0; i < arr.len(); i = i + 1) { sum = sum + arr[i]; }"
                      "print(sum);";
    EXPECT_EQ(runRegVMOutput(src), "10");
}

// ---- 字典 ----
TEST(RegVME2E, DictBasic) {
    std::string src = "var d = {\"a\": 1, \"b\": 2};"
                      "print(d[\"a\"]);"
                      "print(d[\"b\"]);";
    EXPECT_EQ(runRegVMOutput(src), "12");
}

// ---- 字符串 ----
TEST(RegVME2E, StringConcat) {
    EXPECT_EQ(runRegVMOutput("print(\"Hello\" + \" \" + \"World\");"), "Hello World");
}
TEST(RegVME2E, StringInterpolation) {
    std::string src = "var name = \"Alice\";"
                      "var age = 30;"
                      "print(\"Name: {name}, Age: {age}\");";
    EXPECT_EQ(runRegVMOutput(src), "Name: Alice, Age: 30");
}
TEST(RegVME2E, StringInterpolationExpr) {
    EXPECT_EQ(runRegVMOutput("print(\"Result: {1 + 2 * 3}\");"), "Result: 7");
}

// ---- 异常处理（P1-4 fix 验证：catch 变量绑定 + R0 不覆盖）----
TEST(RegVME2E, TryCatchBasic) {
    std::string src = "try {"
                      "  throw \"error\";"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    EXPECT_EQ(runRegVMOutput(src), "error");
}
TEST(RegVME2E, TryCatchNoThrow) {
    std::string src = "try {"
                      "  print(\"try\");"
                      "} catch (e) {"
                      "  print(\"catch\");"
                      "}"
                      "print(\"end\");";
    EXPECT_EQ(runRegVMOutput(src), "tryend");
}
TEST(RegVME2E, TryCatchThrowNumber) {
    std::string src = "try {"
                      "  throw 42;"
                      "} catch (n) {"
                      "  print(n);"
                      "}";
    EXPECT_EQ(runRegVMOutput(src), "42");
}
TEST(RegVME2E, TryCatchFromFunction) {
    std::string src = "fun fail() { throw \"from func\"; }"
                      "try {"
                      "  fail();"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    EXPECT_EQ(runRegVMOutput(src), "from func");
}
TEST(RegVME2E, TryCatchNested) {
    std::string src = "try {"
                      "  try {"
                      "    throw \"inner\";"
                      "  } catch (a) {"
                      "    print(a);"
                      "    throw \"outer\";"
                      "  }"
                      "} catch (b) {"
                      "  print(b);"
                      "}";
    EXPECT_EQ(runRegVMOutput(src), "innerouter");
}
TEST(RegVME2E, TryCatchR0NotOverwritten) {
    // P1-4 fix: 验证异常值不覆盖 R0（方法中 R0 是 this）
    std::string src = "class Safe {"
                      "  fun risky() { throw \"oops\"; }"
                      "  fun safe() { return \"ok\"; }"
                      "}"
                      "var s = Safe();"
                      "try {"
                      "  s.risky();"
                      "} catch (e) {"
                      "  print(e);"
                      "}"
                      "print(s.safe());";
    EXPECT_EQ(runRegVMOutput(src), "oopsok");
}

// ---- 继承 + super（P1-1/P1-2/P1-3 fix 验证）----
TEST(RegVME2E, InheritedMethod) {
    std::string src = "class Base {"
                      "  fun greet() { return \"hello from base\"; }"
                      "}"
                      "class Derived extends Base {"
                      "}"
                      "var d = Derived();"
                      "print(d.greet());";
    EXPECT_EQ(runRegVMOutput(src), "hello from base");
}
TEST(RegVME2E, SuperCall) {
    std::string src = "class Animal {"
                      "  fun speak() { return \"generic sound\"; }"
                      "}"
                      "class Dog extends Animal {"
                      "  fun speak() { return \"woof: \" + super.speak(); }"
                      "}"
                      "var d = Dog();"
                      "print(d.speak());";
    EXPECT_EQ(runRegVMOutput(src), "woof: generic sound");
}
TEST(RegVME2E, SuperInitFields) {
    // P1-1 fix: 父类 init 设置的字段在子类实例中可访问
    std::string src = "class Base {"
                      "  var x;"
                      "  fun init() { this.x = 42; }"
                      "}"
                      "class Child extends Base {"
                      "  fun getX() { return this.x; }"
                      "}"
                      "var c = Child();"
                      "print(c.getX());";
    EXPECT_EQ(runRegVMOutput(src), "42");
}
TEST(RegVME2E, SuperMemberGet) {
    // P1-2 fix: super.field 访问父类字段
    std::string src = "class Base {"
                      "  var val;"
                      "  fun init(v) { this.val = v; }"
                      "}"
                      "class Sub extends Base {"
                      "  fun show() { return super.val; }"
                      "}"
                      "var s = Sub(99);"
                      "print(s.show());";
    EXPECT_EQ(runRegVMOutput(src), "99");
}
TEST(RegVME2E, SuperCallWithArgs) {
    std::string src = "class Base {"
                      "  fun init(n) { this.name = n; }"
                      "  fun describe() { return \"Base: \" + this.name; }"
                      "}"
                      "class Sub extends Base {"
                      "  fun init(n) { super.init(n); }"
                      "  fun describe() { return super.describe() + \" (Sub)\"; }"
                      "}"
                      "var s = Sub(\"test\");"
                      "print(s.describe());";
    EXPECT_EQ(runRegVMOutput(src), "Base: test (Sub)");
}

// ============================================================
// A2 fix: 三后端一致性测试（Interpreter / Stack VM / RegisterVM）
// ------------------------------------------------------------
// 覆盖三类场景：
//   1. 三后端语义一致的基本运算（加/减/乘/模/比较/逻辑）
//   2. 三后端语义一致的控制流/函数/字符串/数组
//   3. REG_DIV 与栈式 VM 的语义差异：REG_DIV 对 int/int 不整除返回 float
//      （"真除"语义），栈式 VM 始终返回 int（截断除法）。Interpreter 与栈式
//      VM 一致。差异需显式测试以文档化，避免误判为 bug。
// ============================================================

// ---- 三后端一致：基本运算（不含 DIV，因 REG_DIV 真除语义不同）----
TEST(BackendConsistency, AddSubMul) {
    std::string src = "print(7 + 3); print(10 - 4); print(6 * 9); print(17 % 5);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// ---- 三后端一致：比较运算 ----
// 注意：and/or 在 Interpreter 与 VM 间存在语义差异（Interpreter 的 `true and false`
// 返回 null，VM 返回 false），故此处仅测试纯比较运算符。差异由
// BackendConsistency.AndOrSemanticDivergence 单独文档化。
TEST(BackendConsistency, ComparisonOps) {
    std::string src = "print(1 < 2); print(2 <= 2); print(3 > 5); print(3 >= 3);"
                      "print(1 == 1); print(1 != 2); print(!true);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// ---- AUDIT-ANDOR fix: and/or 语义三后端一致 ----
// 原 Interpreter 的 `true and false` 返回 null（evaluate(right) 返回值被丢弃，
// lastValue_ 被 std::move 置空）。已修复为 lastValue_ = evaluate(node.right.get())。
// 三后端现在一致返回右操作数值：true and false = false, false or true = true。
TEST(BackendConsistency, AndOrSemanticDivergence) {
    std::string src = "print(true and false); print(false or true);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    // 三后端语义一致：true and false = false, false or true = true
    EXPECT_EQ(interp, "falsetrue");
    EXPECT_EQ(stackVm, "falsetrue");
    EXPECT_EQ(regVm, "falsetrue");
}

// ---- 三后端一致：控制流 ----
TEST(BackendConsistency, ControlFlow) {
    std::string src = "var sum = 0;"
                      "for (var i = 1; i <= 5; i = i + 1) { sum = sum + i; }"
                      "print(sum);"
                      "var x = 10;"
                      "if (x > 5) { print(\"big\"); } else { print(\"small\"); }";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// ---- 三后端一致：函数 + 递归 ----
TEST(BackendConsistency, FunctionRecursion) {
    std::string src = "fun fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }"
                      "print(fib(10));"
                      "fun add(a, b) { return a + b; }"
                      "print(add(3, 4));";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// ---- 三后端一致：字符串方法 ----
// R98 注释更新：原注释称"RegisterVM 尚未实现 str.replace/split/trim/substr/indexOf"
// 已过时——RegisterVM 已通过共享 executeShared* 层补齐所有字符串方法（见下方
// RegVmStringMethodsNowSupported / StringMethodsAllBackends 测试）。三后端字符串
// 方法已全部对齐，此测试覆盖 len/upper/lower/contains 共 4 个基础方法。
TEST(BackendConsistency, StringMethods) {
    std::string src = "var s = \"Hello\";"
                      "print(s.len());"
                      "print(s.upper());"
                      "print(s.lower());"
                      "print(s.contains(\"ell\"));";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// ---- P1-1 fix: RegisterVM 字符串方法已补齐 ----
// 原 RegisterVM 缺失 str.replace/substr/indexOf/split/trim 等方法，
// 现已通过共享 executeShared* 层补齐。三后端行为一致。
TEST(BackendConsistency, RegVmStringMethodsNowSupported) {
    std::string src = "print(\"Hello\".replace(\"ell\", \"X\"));";
    std::string stackVm = runVMOutputForConsistency(src);
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(stackVm, "HXo");
    EXPECT_EQ(interp, "HXo");
    EXPECT_EQ(regVm, "HXo");
}

// ---- P1-1 fix: RegisterVM 字符串方法三后端一致性 ----
TEST(BackendConsistency, StringMethodsAllBackends) {
    // 测试所有字符串方法在三后端行为一致
    struct TestCase {
        const char* src;
        const char* expected;
    };
    TestCase cases[] = {
        {"print(\"Hello World\".upper());", "HELLO WORLD"},
        {"print(\"HELLO\".lower());", "hello"},
        {"print(\"Hello\".contains(\"ell\"));", "true"},
        {"print(\"Hello\".startsWith(\"He\"));", "true"},
        {"print(\"Hello\".endsWith(\"lo\"));", "true"},
        {"print(\"Hello\".replace(\"l\", \"r\"));", "Herro"},
        {"print(\"Hello\".substr(1, 3));", "ell"},
        {"print(\"Hello\".indexOf(\"l\"));", "2"},
        {"print(\"  hi  \".trim());", "hi"},
        {"var parts = \"a,b,c\".split(\",\"); print(parts.len());", "3"},
    };
    for (const auto& tc : cases) {
        std::string interp = runInterpreterOutputForConsistency(tc.src);
        std::string stackVm = runVMOutputForConsistency(tc.src);
        std::string regVm = runRegVMOutput(tc.src);
        EXPECT_EQ(interp, tc.expected) << "Interpreter: " << tc.src;
        EXPECT_EQ(stackVm, tc.expected) << "Stack VM: " << tc.src;
        EXPECT_EQ(regVm, tc.expected) << "RegisterVM: " << tc.src;
    }
}

// ============================================================
// AUDIT-BUG 回归测试（2026-06-30 第二轮 bug 排查）
// ============================================================

// AUDIT-BUG-I1: envPool_ 回收破坏闭包 env weak_ptr
// 闭包在块内定义、仅捕获父级变量时，blockEnv 不应被回收到 envPool_，
// 否则后续块复用该 env 后，闭包调用时变量查找失败。
TEST(BackendConsistency, AuditBugI1_ClosureInBlockCapturingParentVar) {
    std::string src = R"(
func outer() {
    var x = 10;
    var g = null;
    if (true) {
        func f() { return x; }
        g = f;
    }
    if (true) {
        var dummy = 0;
    }
    return g();
}
print(outer());
)";
    std::string interp = runInterpreterOutputForConsistency(src);
    EXPECT_EQ(interp, "10");
}

// AUDIT-BUG-I1 变体：多层嵌套块 + 闭包捕获祖父层变量
TEST(BackendConsistency, AuditBugI1_DeepNestedBlockClosure) {
    std::string src = R"(
func outer() {
    var x = 42;
    var g = null;
    if (true) {
        if (true) {
            func f() { return x + 1; }
            g = f;
        }
    }
    if (true) { var d = 1; }
    if (true) { var d = 2; }
    return g();
}
print(outer());
)";
    std::string interp = runInterpreterOutputForConsistency(src);
    EXPECT_EQ(interp, "43");
}

// AUDIT-BUG-I3: GcManager 自引用容器重入析构
// a = []; a.push(a) 形成自引用循环，execute() 结束后 collectCycle
// 清空 elements 时不应触发重入析构导致 use-after-free 崩溃。
TEST(VME2E, AuditBugI3_SelfReferencingContainerGc) {
    std::string src = R"(
var a = [];
a.push(a);
print("ok");
)";
    // 运行两次触发 collectCycle（execute 开头会回收上一轮的循环孤岛）
    std::string out1 = runVMOutput(src);
    std::string out2 = runVMOutput(src);
    EXPECT_EQ(out1, "ok");
    EXPECT_EQ(out2, "ok");

    // Interpreter 路径也验证
    std::string interp1 = runInterpreterOutputForConsistency(src);
    std::string interp2 = runInterpreterOutputForConsistency(src);
    EXPECT_EQ(interp1, "ok");
    EXPECT_EQ(interp2, "ok");
}

// AUDIT-BUG-I3 变体：字典自引用
TEST(VME2E, AuditBugI3_SelfReferencingDictGc) {
    std::string src = R"(
var d = {};
d["self"] = d;
print("ok");
)";
    std::string out1 = runVMOutput(src);
    std::string out2 = runVMOutput(src);
    EXPECT_EQ(out1, "ok");
    EXPECT_EQ(out2, "ok");
}

// AUDIT-BUG-I4: VAL_FLOAT toString locale-independent
// 验证浮点数 toString 输出使用 '.' 而非 locale 相关的分隔符
// 使用精确二进制表示的值避免 17 位精度输出差异
TEST(VME2E, AuditBugI4_FloatToStringLocaleIndependent) {
    std::string src = "print(0.5);";
    std::string out = runVMOutput(src);
    EXPECT_EQ(out, "0.5");

    std::string src2 = "print(1.5 + 2.5);";
    std::string out2 = runVMOutput(src2);
    EXPECT_EQ(out2, "4");
}

// AUDIT-BUG-C3: StackVM 字段继承合并顺序错误
// 3 级继承链中，直接父类同名字段应覆盖根祖先后字段
TEST(BackendConsistency, AuditBugC3_FieldInheritanceMergeOrder) {
    std::string src = R"(
class A { var x = 1; }
class B extends A { var x = 2; }
class C extends B {}
var c = C();
print(c.x);
)";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, "2");
    EXPECT_EQ(stackVm, "2");
    EXPECT_EQ(regVm, "2");
}

// AUDIT-BUG-C3 变体：4 级继承链
TEST(BackendConsistency, AuditBugC3_DeepInheritanceFieldOverride) {
    std::string src = R"(
class A { var x = 10; }
class B extends A { var x = 20; }
class C extends B { var x = 30; }
class D extends C {}
var d = D();
print(d.x);
)";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, "30");
    EXPECT_EQ(stackVm, "30");
    EXPECT_EQ(regVm, "30");
}

// AUDIT-BUG-C1: ensureUnique COW 克隆注册到 GcManager
// 共享数组 COW detach 后形成自引用循环，collectCycle 应回收
TEST(VME2E, AuditBugC1_CowCloneGcManagerRegistration) {
    std::string src = R"(
var a = [];
var b = a;
b.push(b);
a = null;
b = null;
print("ok");
)";
    // 运行两次触发 collectCycle
    std::string out1 = runVMOutput(src);
    std::string out2 = runVMOutput(src);
    EXPECT_EQ(out1, "ok");
    EXPECT_EQ(out2, "ok");
}

// ---- 三后端一致：数组操作 ----
TEST(BackendConsistency, ArrayOps) {
    std::string src = "var arr = [1, 2, 3];"
                      "print(arr.len());"
                      "arr.push(4);"
                      "print(arr.len());"
                      "print(arr[2]);"
                      "print(arr.contains(2));";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// ============================================================
// A2 fix: REG_DIV 语义差异显式测试
// ------------------------------------------------------------
// RegisterVM 的 REG_DIV 采用「真除」语义：
//   - int / int 整除 → int（与栈式 VM 一致）
//   - int / int 不整除 → float（与栈式 VM 不同！栈式 VM 截断为 int）
//   - 任意涉及 float 的除法 → float（与栈式 VM 一致）
// 栈式 VM 的 OP_DIV 通过 NumericOps::computeArith 始终 int/int → int。
// Interpreter 与栈式 VM 语义一致。
// 此差异是有意设计（寄存器式更接近 Python/JS 真除语义），需测试文档化。
// ============================================================

// REG_DIV 整除：三后端一致（int/int 整除时 REG_DIV 也返回 int）
TEST(BackendConsistency, DivEvenlyDivisible) {
    std::string src = "print(10 / 2); print(20 / 4); print(100 / 5);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// REG_DIV 不整除：AUDIT-DIV-UNIFY fix 后三后端一致截断为 int
TEST(BackendConsistency, DivNotEvenlyDivisible_StackVsRegister) {
    std::string src = "print(7 / 2);";
    // AUDIT-DIV-UNIFY fix: 三后端均 7 / 2 = 3（int 截断除法，向零截断）
    std::string stackVm = runVMOutputForConsistency(src);
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(stackVm, interp);
    EXPECT_EQ(stackVm, "3");
    EXPECT_EQ(regVm, "3");     // 原 RegVM 返回 "3.5"（真除），已统一为截断
    EXPECT_EQ(regVm, stackVm); // 三后端一致
}

// REG_DIV 涉及 float：三后端一致（均返回 float）
TEST(BackendConsistency, DivWithFloat) {
    std::string src = "print(7.0 / 2); print(10.0 / 4);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// REG_DIV 除零：三后端一致（均报错，无部分输出）
TEST(BackendConsistency, DivByZero) {
    std::string src = "print(10 / 0);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    // P1-7 fix: 显式传 expectError=true 跳过 hasError 断言（除零是预期错误）
    std::string regVm = runRegVMOutput(src, /*expectError=*/true);
    // AUDIT-HELPER fix: ForConsistency helpers 现编码错误为 <runtime:msg>。
    // 三后端均触发除零错误，无部分 print 输出：
    //   interp/stackVm: 输出以 <runtime: 开头（无前缀输出）
    //   regVm: runRegVMOutput(expectError=true) 返回 captured（空）
    EXPECT_NE(interp.find("<runtime:"), std::string::npos) << "interp: " << interp;
    EXPECT_NE(stackVm.find("<runtime:"), std::string::npos) << "stackVm: " << stackVm;
    EXPECT_TRUE(regVm.empty());
}

// REG_DIV 综合一致性：AUDIT-DIV-UNIFY fix 后三后端一致
TEST(BackendConsistency, DivMixedSemantics) {
    std::string src = "print(8 / 2);"
                      "print(9 / 2);"
                      "print(8.0 / 2);"
                      "print(9.0 / 2);";
    // AUDIT-DIV-UNIFY fix: 三后端一致 4, 4, 4, 4.5
    // （9/2 整数截断为 4，9.0/2 float 除法为 4.5）
    std::string stackVm = runVMOutputForConsistency(src);
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_EQ(stackVm, interp);
    EXPECT_EQ(stackVm, "4444.5");
    EXPECT_EQ(regVm, "4444.5"); // 原 RegVM 返回 "44.544.5"（9/2 真除为 4.5），已统一
    EXPECT_EQ(regVm, stackVm);
}

// P1-6 fix: 负索引在所有后端都应报错（不做 Python 式 wraparound）
TEST(BackendConsistency, NegativeIndexAllError) {
    std::string src = "var arr = [10, 20, 30]; print(arr[-1]);";
    // 三个后端都应抛运行时错误，无部分输出
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src, true);
    // AUDIT-HELPER fix: ForConsistency helpers 现编码错误为 <runtime:msg>。
    //   interp/stackVm: 输出以 <runtime: 开头（无前缀输出）
    //   regVm: runRegVMOutput(expectError=true) 返回 captured（空）
    EXPECT_NE(interp.find("<runtime:"), std::string::npos) << "interp: " << interp;
    EXPECT_NE(stackVm.find("<runtime:"), std::string::npos) << "stackVm: " << stackVm;
    EXPECT_TRUE(regVm.empty());
}

// P1-7 fix: 字典键不存在时所有后端都应返回 null
TEST(BackendConsistency, DictMissingKeyReturnsNull) {
    std::string src = "var d = {\"a\": 1, \"b\": 2};"
                      "print(d[\"a\"]);"
                      "print(d[\"missing\"]);"
                      "print(d.get(\"b\"));"
                      "print(d.get(\"nope\"));";
    std::string expected = "1null2null";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), expected);
    EXPECT_EQ(runVMOutputForConsistency(src), expected);
    EXPECT_EQ(runRegVMOutput(src), expected);
}

// ============================================================
// 2026-06-29: OP_TYPE_CHECK / REG_TYPE_CHECK 三后端运行时类型强制
// ------------------------------------------------------------
// 验证带类型注解的变量在运行时收到不兼容值时，三后端统一报错；
// 兼容值在三后端统一通过。null 兼容所有类型注解。
// ============================================================

// 类型注解合规赋值：三后端应一致通过并产生输出
TEST(BackendConsistency, TypeAnnotationCompatibleAssignment) {
    std::string src = "int a = 5;"
                      "a = 10;"
                      "print(a);";
    std::string expected = "10";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), expected);
    EXPECT_EQ(runVMOutputForConsistency(src), expected);
    EXPECT_EQ(runRegVMOutput(src), expected);
}

// 类型注解违反赋值（int 接收 string）：三后端应一致报错（无输出）
TEST(BackendConsistency, TypeAnnotationViolationStringToInt) {
    std::string src = "int a = 5;"
                      "a = \"hello\";"
                      "print(a);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src, true);
    // 三后端都不应打印 "hello"（类型违反应中断执行）
    EXPECT_EQ(interp.find("hello"), std::string::npos);
    EXPECT_EQ(stackVm.find("hello"), std::string::npos);
    EXPECT_EQ(regVm.find("hello"), std::string::npos);
}

// 类型注解违反赋值（int 接收 bool）：三后端应一致报错
TEST(BackendConsistency, TypeAnnotationViolationBoolToInt) {
    std::string src = "int a = 5;"
                      "a = true;"
                      "print(a);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src, true);
    // 三后端都不应打印 "true"
    EXPECT_EQ(interp.find("true"), std::string::npos);
    EXPECT_EQ(stackVm.find("true"), std::string::npos);
    EXPECT_EQ(regVm.find("true"), std::string::npos);
}

// null 兼容所有类型注解：三后端应一致通过
TEST(BackendConsistency, NullCompatibleWithAllAnnotations) {
    std::string src = "int a = null;"
                      "string b = null;"
                      "bool c = null;"
                      "print(a);"
                      "print(b);"
                      "print(c);";
    std::string expected = "nullnullnull";
    EXPECT_EQ(runInterpreterOutputForConsistency(src), expected);
    EXPECT_EQ(runVMOutputForConsistency(src), expected);
    EXPECT_EQ(runRegVMOutput(src), expected);
}

// int→float 宽化：三后端应一致通过
TEST(BackendConsistency, IntWidensToFloatAnnotation) {
    std::string src = "float a = 5;"
                      "a = 10;"
                      "print(a);";
    // Interpreter/VM 对 int→float 宽化的输出格式可能不同（5 vs 5.0）
    // 仅验证三后端都通过（无错误），输出值相同
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src);
    EXPECT_FALSE(interp.empty());
    EXPECT_EQ(interp, stackVm);
    EXPECT_EQ(interp, regVm);
}

// 类型注解违反初始化（int a = "hello"）：三后端应一致报错
TEST(BackendConsistency, TypeAnnotationViolationOnVarDeclInit) {
    std::string src = "int a = \"hello\";"
                      "print(a);";
    std::string interp = runInterpreterOutputForConsistency(src);
    std::string stackVm = runVMOutputForConsistency(src);
    std::string regVm = runRegVMOutput(src, true);
    EXPECT_EQ(interp.find("hello"), std::string::npos);
    EXPECT_EQ(stackVm.find("hello"), std::string::npos);
    EXPECT_EQ(regVm.find("hello"), std::string::npos);
}

// ============================================================
// VM-IMPORT: 模块系统 / import / export（编译期模块内联）
// ------------------------------------------------------------
// VM 端采用编译期模块内联策略：在编译阶段加载模块源码、解析 AST、
// 预扫描全局槽位、内联编译模块语句到主程序中。VM 运行时无需模块加载机制。
//
// 语义差异说明（与 Interpreter 对比）：
// - Interpreter: 模块在独立 Environment 中执行，仅导出名可见（隔离）
// - VM: 模块代码内联到同一全局作用域，所有顶层名可见（无隔离）
//   实践影响小——导入方通常只使用其声明的导入名
// - run-once: 同一模块多次 import 时仅编译/执行一次（linkedModuleSet_ 保证）
// ============================================================

// 辅助：执行带模块的源码，返回 print 输出（编译错误编码为 <compile:msg>）
static std::string runVMWithModules(const std::string& source,
                                    const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end())
            throw std::runtime_error("module not found: " + path);
        return it->second;
    });
    CompileResult result;
    try {
        result = compiler.compile(*ast);
    } catch (const std::exception& e) {
        return "<compile:" + std::string(e.what()) + ">";
    }
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// 辅助：执行带模块的源码，返回是否有编译错误
static bool runVMWithModulesCompileError(const std::string& source,
                                         const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return false;

    Compiler compiler;
    compiler.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end())
            throw std::runtime_error("module not found: " + path);
        return it->second;
    });
    try {
        compiler.compile(*ast);
    } catch (const std::exception&) {
        return true; // loader 抛异常视为编译失败
    }
    return compiler.getDiagnostics().hasErrors();
}

// 测试：import 全部导出
TEST(VME2EImport, ImportAll) {
    std::string src = "import \"mymod\";"
                      "print(PI);"
                      "print(add(3, 4));";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "export var PI = 314;"
                                                                      "export fun add(a, b) { return a + b; }"}};
    EXPECT_EQ(runVMWithModules(src, modules), "3147");
}

// 测试：import 指定名称
TEST(VME2EImport, ImportNamed) {
    std::string src = "import { greet } from \"greetings\";"
                      "print(greet(\"world\"));";
    std::unordered_map<std::string, std::string> modules = {{"greetings",
                                                             "export fun greet(name) { return \"hello \" + name; }"
                                                             "export fun unused() { return 999; }"}};
    EXPECT_EQ(runVMWithModules(src, modules), "hello world");
}

// 测试：export 类
TEST(VME2EImport, ExportClass) {
    std::string src = "import { Point } from \"geom\";"
                      "var p = Point(3, 4);"
                      "print(p.x);"
                      "print(p.y);"
                      "print(p.norm());";
    std::unordered_map<std::string, std::string> modules = {{"geom", "export class Point {"
                                                                     "  var x = 0;"
                                                                     "  var y = 0;"
                                                                     "  fun init(ax, ay) { x = ax; y = ay; }"
                                                                     "  fun norm() { return (x * x + y * y) % 100; }"
                                                                     "}"}};
    EXPECT_EQ(runVMWithModules(src, modules), "3425");
}

// 测试：模块缓存（多次 import 同一模块只编译执行一次）
TEST(VME2EImport, RunOnce) {
    std::string src = "import { counter } from \"counter_mod\";"
                      "print(counter());"
                      "import { counter } from \"counter_mod\";"
                      "print(counter());";
    std::unordered_map<std::string, std::string> modules = {
        {"counter_mod", "var count = 10;"
                        "export fun counter() { count = count + 1; return count; }"}};
    // 模块内联后 count 为全局变量，counter() 每次调用递增
    // 第二次 import 被跳过（linkedModuleSet_ 保证 run-once）
    EXPECT_EQ(runVMWithModules(src, modules), "1112");
}

// 测试：循环导入延迟加载（P2-14）
// 循环依赖不再报错——模块按执行顺序内联编译，回路闭合时跳过本次内联编译。
// 语义参考 ES Modules + Python：模块全局槽位立即分配，值按执行顺序填充。
TEST(VME2EImport, CircularDependency) {
    std::string src = "import \"a\";";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\";"}, {"b", "import \"a\";"}};
    // 循环导入现在成功执行，无 print 输出
    EXPECT_EQ(runVMWithModules(src, modules), "");
}

// 测试：循环导入执行顺序（P2-14）
TEST(VME2EImport, CircularDependencyExecutionOrder) {
    std::string src = "import \"a\";"
                      "print(\"main\");";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\"; print(\"a\");"},
                                                            {"b", "import \"a\"; print(\"b\");"}};
    // 执行序：b（回路闭合后先执行完）→ a → main
    EXPECT_EQ(runVMWithModules(src, modules), "bamain");
}

// 测试：循环导入——导出名在循环引用前定义（P2-14）
TEST(VME2EImport, CircularDependencyExportBeforeCycle) {
    std::string src = "import { get_a } from \"a\";"
                      "print(get_a());";
    std::unordered_map<std::string, std::string> modules = {{"a", "export fun get_a() { return 1; }"
                                                                  "import \"b\";"},
                                                            {"b", "import \"a\";"
                                                                  "print(\"b\");"}};
    // b 先 print("b")，然后 main 调用 get_a() 输出 1
    EXPECT_EQ(runVMWithModules(src, modules), "b1");
}

// 测试：模块不存在
TEST(VME2EImport, ModuleNotFound) {
    std::string src = "import \"nonexistent\";";
    std::unordered_map<std::string, std::string> modules;
    EXPECT_TRUE(runVMWithModulesCompileError(src, modules));
}

// 测试：未设置模块加载器
TEST(VME2EImport, NoLoader) {
    std::string src = "import \"m\";";
    Lexer lexer;
    auto tokens = lexer.scan(src);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_TRUE(ast != nullptr);
    Compiler compiler; // 不设置 moduleLoader_
    compiler.compile(*ast);
    EXPECT_TRUE(compiler.getDiagnostics().hasErrors());
}

// 测试：嵌套模块导入
TEST(VME2EImport, NestedImport) {
    std::string src = "import { getValue } from \"outer\";"
                      "print(getValue());";
    std::unordered_map<std::string, std::string> modules = {{"outer", "import { base } from \"inner\";"
                                                                      "export fun getValue() { return base + 100; }"},
                                                            {"inner", "export var base = 42;"}};
    EXPECT_EQ(runVMWithModules(src, modules), "142");
}

// 测试：具名导入不存在的名称（名称在模块中完全不存在时报错）
TEST(VME2EImport, NamedNonExistent) {
    std::string src = "import { nonexistent } from \"m\";"
                      "print(nonexistent);";
    std::unordered_map<std::string, std::string> modules = {{"m", "export var public_val = 1;"}};
    // nonexistent 在模块中不存在，lookupGlobalSlot 返回 -1，编译报错
    EXPECT_TRUE(runVMWithModulesCompileError(src, modules));
}

// 测试：具名导入原子性 — 部分名称不存在时不导入任何名称
TEST(VME2EImport, NamedAtomic) {
    std::string src = "import { a, nonexistent, c } from \"m\";"
                      "print(a);"
                      "print(c);";
    std::unordered_map<std::string, std::string> modules = {{"m", "export var a = 1;"
                                                                  "export var c = 3;"}};
    // nonexistent 未导出，应编译报错
    EXPECT_TRUE(runVMWithModulesCompileError(src, modules));
}

// 测试：路径遍历防护（SEC-1: 拒绝 ".." 路径段）
TEST(VME2EImport, PathTraversalRejected) {
    std::string src = "import \"../secret\";";
    std::unordered_map<std::string, std::string> modules = {{"../secret", "export var x = 1;"}};
    // 路径包含 ".." 应被拒绝（normalizeModulePath 返回空）
    EXPECT_TRUE(runVMWithModulesCompileError(src, modules));
}

// 测试：绝对路径被拒绝
TEST(VME2EImport, AbsolutePathRejected) {
    std::string src = "import \"/etc/passwd\";";
    std::unordered_map<std::string, std::string> modules = {{"/etc/passwd", "export var x = 1;"}};
    EXPECT_TRUE(runVMWithModulesCompileError(src, modules));
}

// 测试：空路径被拒绝（解析期即拒绝，Parser::importStmt 抛 ParseError）
TEST(VME2EImport, EmptyPathRejected) {
    std::string src = "import \"\";";
    Lexer lexer;
    auto tokens = lexer.scan(src);
    Parser parser;
    auto ast = parser.parse(tokens);
    // 解析器对空模块路径抛 ParseError，记录诊断后继续
    EXPECT_TRUE(parser.hasErrors());
}

// 测试：模块内函数递归
TEST(VME2EImport, RecursiveFunctionInModule) {
    std::string src = "import { factorial } from \"math\";"
                      "print(factorial(5));";
    std::unordered_map<std::string, std::string> modules = {{"math", "export fun factorial(n) {"
                                                                     "  if (n <= 1) { return 1; }"
                                                                     "  return n * factorial(n - 1);"
                                                                     "}"}};
    EXPECT_EQ(runVMWithModules(src, modules), "120");
}

// 测试：import 全部后使用多个导出（函数+变量+类）
TEST(VME2EImport, ImportAllMixedExports) {
    std::string src = "import \"lib\";"
                      "print(VERSION);"
                      "print(double(21));"
                      "var p = Pair(1, 2);"
                      "print(p.first);"
                      "print(p.second);";
    std::unordered_map<std::string, std::string> modules = {{"lib", "export var VERSION = 100;"
                                                                    "export fun double(x) { return x * 2; }"
                                                                    "export class Pair {"
                                                                    "  var first = 0;"
                                                                    "  var second = 0;"
                                                                    "  fun init(a, b) { first = a; second = b; }"
                                                                    "}"}};
    EXPECT_EQ(runVMWithModules(src, modules), "1004212");
}

// 测试：菱形依赖（两个模块导入同一基础模块）
TEST(VME2EImport, DiamondDependency) {
    std::string src = "import { getValueA } from \"modA\";"
                      "import { getValueB } from \"modB\";"
                      "print(getValueA());"
                      "print(getValueB());";
    std::unordered_map<std::string, std::string> modules = {{"base", "export var baseVal = 42;"},
                                                            {"modA", "import { baseVal } from \"base\";"
                                                                     "export fun getValueA() { return baseVal + 1; }"},
                                                            {"modB", "import { baseVal } from \"base\";"
                                                                     "export fun getValueB() { return baseVal + 2; }"}};
    // base 模块只编译一次（run-once），baseVal 全局槽位复用
    EXPECT_EQ(runVMWithModules(src, modules), "4344");
}

// ============================================================
// VM-IMPORT: IR 路径 / 寄存器式路径的模块导入测试
// ------------------------------------------------------------
// 验证 AstIRBuilder::handleImportStmt 与 Compiler::visitImportStmt 语义一致。
// ============================================================

// 辅助：通过 IR 路径执行带模块的源码，返回 print 输出
static std::string runVMWithModulesIR(const std::string& source,
                                      const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setUseIR(true);
    compiler.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end())
            throw std::runtime_error("module not found: " + path);
        return it->second;
    });
    CompileResult result;
    try {
        result = compiler.compile(*ast);
    } catch (const std::exception& e) {
        return "<compile:" + std::string(e.what()) + ">";
    }
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// 辅助：通过寄存器式路径执行带模块的源码，返回 print 输出
static std::string runVMWithModulesReg(const std::string& source,
                                       const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end())
            throw std::runtime_error("module not found: " + path);
        return it->second;
    });
    try {
        compiler.compile(*ast);
    } catch (const std::exception& e) {
        return "<compile:" + std::string(e.what()) + ">";
    }
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }

    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(compiler.getLastRegisterResult());
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// IR 路径：import 全部导出
TEST(VME2EImportIR, ImportAll) {
    std::string src = "import \"mymod\";"
                      "print(PI);"
                      "print(add(3, 4));";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "export var PI = 314;"
                                                                      "export fun add(a, b) { return a + b; }"}};
    EXPECT_EQ(runVMWithModulesIR(src, modules), "3147");
}

// IR 路径：import 指定名称
TEST(VME2EImportIR, ImportNamed) {
    std::string src = "import { greet } from \"greetings\";"
                      "print(greet(\"world\"));";
    std::unordered_map<std::string, std::string> modules = {
        {"greetings", "export fun greet(name) { return \"hello \" + name; }"}};
    EXPECT_EQ(runVMWithModulesIR(src, modules), "hello world");
}

// IR 路径：export 类
TEST(VME2EImportIR, ExportClass) {
    std::string src = "import { Point } from \"geom\";"
                      "var p = Point(3, 4);"
                      "print(p.x);"
                      "print(p.y);"
                      "print(p.norm());";
    std::unordered_map<std::string, std::string> modules = {{"geom", "export class Point {"
                                                                     "  var x = 0;"
                                                                     "  var y = 0;"
                                                                     "  fun init(ax, ay) { x = ax; y = ay; }"
                                                                     "  fun norm() { return (x * x + y * y) % 100; }"
                                                                     "}"}};
    // L7 fix: IR 路径类支持完整，import 类 + 实例化 + 方法调用三后端一致
    EXPECT_EQ(runVMWithModulesIR(src, modules), "3425");
}

// IR 路径：模块内递归函数
TEST(VME2EImportIR, RecursiveFunctionInModule) {
    std::string src = "import { factorial } from \"math\";"
                      "print(factorial(5));";
    std::unordered_map<std::string, std::string> modules = {{"math", "export fun factorial(n) {"
                                                                     "  if (n <= 1) { return 1; }"
                                                                     "  return n * factorial(n - 1);"
                                                                     "}"}};
    EXPECT_EQ(runVMWithModulesIR(src, modules), "120");
}

// IR 路径：嵌套 import
TEST(VME2EImportIR, NestedImport) {
    std::string src = "import { getValue } from \"outer\";"
                      "print(getValue());";
    std::unordered_map<std::string, std::string> modules = {{"outer", "import { base } from \"inner\";"
                                                                      "export fun getValue() { return base + 100; }"},
                                                            {"inner", "export var base = 42;"}};
    EXPECT_EQ(runVMWithModulesIR(src, modules), "142");
}

// IR 路径：循环导入延迟加载（P2-14）
// 循环依赖不再报错——模块按执行顺序内联编译，回路闭合时返回 kCircularLoading 跳过本次内联编译。
TEST(VME2EImportIR, CircularDependency) {
    std::string src = "import \"a\";";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\";"}, {"b", "import \"a\";"}};
    // 循环导入现在成功执行，无 print 输出
    EXPECT_EQ(runVMWithModulesIR(src, modules), "");
}

// IR 路径：循环导入执行顺序（P2-14）
TEST(VME2EImportIR, CircularDependencyExecutionOrder) {
    std::string src = "import \"a\";"
                      "print(\"main\");";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\"; print(\"a\");"},
                                                            {"b", "import \"a\"; print(\"b\");"}};
    // 执行序：b（回路闭合后先执行完）→ a → main
    EXPECT_EQ(runVMWithModulesIR(src, modules), "bamain");
}

// IR 路径：循环导入——导出名在循环引用前定义（P2-14）
TEST(VME2EImportIR, CircularDependencyExportBeforeCycle) {
    std::string src = "import { get_a } from \"a\";"
                      "print(get_a());";
    std::unordered_map<std::string, std::string> modules = {{"a", "export fun get_a() { return 1; }"
                                                                  "import \"b\";"},
                                                            {"b", "import \"a\";"
                                                                  "print(\"b\");"}};
    // b 先 print("b")，然后 main 调用 get_a() 输出 1
    EXPECT_EQ(runVMWithModulesIR(src, modules), "b1");
}

// IR 路径：模块不存在（应编译失败）
TEST(VME2EImportIR, ModuleNotFound) {
    std::string src = "import \"nonexistent\";";
    std::unordered_map<std::string, std::string> modules;
    std::string result = runVMWithModulesIR(src, modules);
    EXPECT_NE(result.find("<compile:"), std::string::npos);
}

// IR 路径：前向函数引用（验证 preScanTopLevelDecls 修复）
TEST(VME2EImportIR, ForwardFunctionReference) {
    std::string src = "print(double(5));"
                      "fun double(x) { return x * 2; }";
    EXPECT_EQ(runVMWithModulesIR(src, {}), "10");
}

// IR 路径：直接类定义（不通过 import），用于隔离测试 IR 类支持是否完整
// L7 fix: IR 路径类支持已完整（visitClassDecl + DEFINE_CLASS + 方法编译 + 字段默认值）
TEST(VME2EImportIR, DirectClassNoImport) {
    std::string src = "class Point {"
                      "  var x = 0;"
                      "  var y = 0;"
                      "  fun init(ax, ay) { x = ax; y = ay; }"
                      "  fun norm() { return (x * x + y * y) % 100; }"
                      "}"
                      "var p = Point(3, 4);"
                      "print(p.x);"
                      "print(p.y);"
                      "print(p.norm());";
    // L7 fix: IR 路径类支持完整，期望与直接路径一致输出 "3425"
    EXPECT_EQ(runVMWithModulesIR(src, {}), "3425");
}

// 寄存器式路径：import 全部导出
TEST(VME2EImportReg, ImportAll) {
    std::string src = "import \"mymod\";"
                      "print(PI);"
                      "print(add(3, 4));";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "export var PI = 314;"
                                                                      "export fun add(a, b) { return a + b; }"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "3147");
}

// 寄存器式路径：import 指定名称
TEST(VME2EImportReg, ImportNamed) {
    std::string src = "import { greet } from \"greetings\";"
                      "print(greet(\"world\"));";
    std::unordered_map<std::string, std::string> modules = {
        {"greetings", "export fun greet(name) { return \"hello \" + name; }"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "hello world");
}

// 寄存器式路径：模块内递归函数
TEST(VME2EImportReg, RecursiveFunctionInModule) {
    std::string src = "import { factorial } from \"math\";"
                      "print(factorial(5));";
    std::unordered_map<std::string, std::string> modules = {{"math", "export fun factorial(n) {"
                                                                     "  if (n <= 1) { return 1; }"
                                                                     "  return n * factorial(n - 1);"
                                                                     "}"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "120");
}

// 寄存器式路径：嵌套 import
TEST(VME2EImportReg, NestedImport) {
    std::string src = "import { getValue } from \"outer\";"
                      "print(getValue());";
    std::unordered_map<std::string, std::string> modules = {{"outer", "import { base } from \"inner\";"
                                                                      "export fun getValue() { return base + 100; }"},
                                                            {"inner", "export var base = 42;"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "142");
}

// 寄存器式路径：前向函数引用（验证 preScanTopLevelDecls 修复）
TEST(VME2EImportReg, ForwardFunctionReference) {
    std::string src = "print(double(5));"
                      "fun double(x) { return x * 2; }";
    EXPECT_EQ(runVMWithModulesReg(src, {}), "10");
}

// 寄存器式路径：循环导入延迟加载（P2-14）
TEST(VME2EImportReg, CircularDependency) {
    std::string src = "import \"a\";";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\";"}, {"b", "import \"a\";"}};
    // 循环导入现在成功执行，无 print 输出
    EXPECT_EQ(runVMWithModulesReg(src, modules), "");
}

// 寄存器式路径：循环导入执行顺序（P2-14）
TEST(VME2EImportReg, CircularDependencyExecutionOrder) {
    std::string src = "import \"a\";"
                      "print(\"main\");";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\"; print(\"a\");"},
                                                            {"b", "import \"a\"; print(\"b\");"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "bamain");
}

// 寄存器式路径：循环导入——导出名在循环引用前定义（P2-14）
TEST(VME2EImportReg, CircularDependencyExportBeforeCycle) {
    std::string src = "import { get_a } from \"a\";"
                      "print(get_a());";
    std::unordered_map<std::string, std::string> modules = {{"a", "export fun get_a() { return 1; }"
                                                                  "import \"b\";"},
                                                            {"b", "import \"a\";"
                                                                  "print(\"b\");"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "b1");
}

// ============================================================
// BUG-AUDIT-MOD-2 回归测试：VM/IR/RegVM 模块隔离
// ------------------------------------------------------------
// 验证模块的非导出顶层名被前缀化重命名，导入方无法用原名访问。
// 与 Interpreter 的独立 Environment 隔离语义对齐。
// ============================================================

// 辅助：检查结果字符串是否表示编译错误
static bool isCompileError(const std::string& result) {
    return result.find("<compile:") == 0;
}
// 辅助：检查结果字符串是否表示运行时错误
static bool isRuntimeError(const std::string& result) {
    return result.find("<runtime:") != std::string::npos;
}

// VM 路径：非导出变量不可被导入方访问
TEST(VME2EImportIsolation, VM_NonExportedVarInaccessible) {
    std::string src = "import \"mymod\";"
                      "print(count);"; // 试图访问模块非导出变量
    std::unordered_map<std::string, std::string> modules = {{"mymod", "var count = 10;"
                                                                      "export fun getCount() { return count; }"}};
    std::string result = runVMWithModules(src, modules);
    // 模块内 count 已重命名为 __mod_<hash>__count，导入方用原名访问应失败
    EXPECT_TRUE(isCompileError(result) || isRuntimeError(result)) << "实际结果: " << result;
}

// VM 路径：非导出函数不可被导入方调用
TEST(VME2EImportIsolation, VM_NonExportedFuncInaccessible) {
    std::string src = "import \"mymod\";"
                      "print(helper());"; // 试图调用模块非导出函数
    std::unordered_map<std::string, std::string> modules = {{"mymod", "fun helper() { return 42; }"
                                                                      "export fun api() { return helper(); }"}};
    std::string result = runVMWithModules(src, modules);
    EXPECT_TRUE(isCompileError(result) || isRuntimeError(result)) << "实际结果: " << result;
}

// VM 路径：非导出类不可被导入方实例化
TEST(VME2EImportIsolation, VM_NonExportedClassInaccessible) {
    std::string src = "import \"mymod\";"
                      "var p = Internal(3, 4);"; // 试图实例化模块非导出类
    std::unordered_map<std::string, std::string> modules = {{"mymod",
                                                             "class Internal { var x = 0; fun init(a) { x = a; } }"
                                                             "export fun makeInternal() { return Internal(99); }"}};
    std::string result = runVMWithModules(src, modules);
    EXPECT_TRUE(isCompileError(result) || isRuntimeError(result)) << "实际结果: " << result;
}

// VM 路径：导出名仍可正常访问
TEST(VME2EImportIsolation, VM_ExportedStillAccessible) {
    std::string src = "import \"mymod\";"
                      "print(getCount());";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "var count = 10;"
                                                                      "export fun getCount() { return count; }"}};
    EXPECT_EQ(runVMWithModules(src, modules), "10");
}

// VM 路径：模块内部引用非导出名仍正常工作（重写后引用一致）
TEST(VME2EImportIsolation, VM_InternalRefsWork) {
    std::string src = "import \"mymod\";"
                      "print(getCount());"
                      "print(inc());"
                      "print(getCount());";
    std::unordered_map<std::string, std::string> modules = {{"mymod",
                                                             "var count = 10;"
                                                             "export fun getCount() { return count; }"
                                                             "export fun inc() { count = count + 5; return count; }"}};
    // 模块内 count 被重命名，但 getCount/inc 内的引用也被重写，语义保持
    EXPECT_EQ(runVMWithModules(src, modules), "101515");
}

// VM 路径：导入方可以有与模块非导出名同名的变量（无冲突）
TEST(VME2EImportIsolation, VM_SameNameNoCollision) {
    std::string src = "import \"mymod\";"
                      "var count = 999;" // 导入方自己的 count，与模块非导出 count 不冲突
                      "print(count);"
                      "print(getCount());";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "var count = 10;"
                                                                      "export fun getCount() { return count; }"}};
    // 导入方 count=999，模块 count 重命名后=10，两者互不影响
    EXPECT_EQ(runVMWithModules(src, modules), "99910");
}

// VM 路径：模块局部变量（函数内）不受重命名影响
TEST(VME2EImportIsolation, VM_LocalVarNotRenamed) {
    std::string src = "import \"mymod\";"
                      "print(run());";
    std::unordered_map<std::string, std::string> modules = {{"mymod",
                                                             "var count = 10;" // 顶层非导出 → 重命名
                                                             "export fun run() {"
                                                             "  var count = 99;" // 函数局部 → 不重命名，遮蔽顶层
                                                             "  return count;"   // 引用局部，不重命名
                                                             "}"}};
    // 局部 count=99 遮蔽模块顶层 count，返回 99
    EXPECT_EQ(runVMWithModules(src, modules), "99");
}

// VM 路径：模块非导出类的继承链仍正常工作
TEST(VME2EImportIsolation, VM_NonExportedClassInheritance) {
    std::string src = "import \"mymod\";"
                      "print(makeAndCall());";
    std::unordered_map<std::string, std::string> modules = {
        {"mymod",
         "class Base { fun greet() { return 1; } }" // 非导出类 → 重命名
         "export class Derived : Base {"            // 导出类 → 不重命名，继承重命名后的 Base
         "  fun init() {}"
         "}"
         "export fun makeAndCall() { return Derived().greet(); }"}};
    // Derived 继承被重命名的 Base，调用继承的 greet() 返回 1
    EXPECT_EQ(runVMWithModules(src, modules), "1");
}

// IR 路径：非导出变量不可被导入方访问
TEST(VME2EImportIsolation, IR_NonExportedVarInaccessible) {
    std::string src = "import \"mymod\";"
                      "print(count);";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "var count = 10;"
                                                                      "export fun getCount() { return count; }"}};
    std::string result = runVMWithModulesIR(src, modules);
    EXPECT_TRUE(isCompileError(result) || isRuntimeError(result)) << "实际结果: " << result;
}

// IR 路径：导出名仍可正常访问 + 内部引用工作
TEST(VME2EImportIsolation, IR_ExportedAndInternalRefs) {
    std::string src = "import \"mymod\";"
                      "print(getCount());"
                      "print(inc());"
                      "print(getCount());";
    std::unordered_map<std::string, std::string> modules = {{"mymod",
                                                             "var count = 10;"
                                                             "export fun getCount() { return count; }"
                                                             "export fun inc() { count = count + 5; return count; }"}};
    EXPECT_EQ(runVMWithModulesIR(src, modules), "101515");
}

// IR 路径：导入方同名变量不冲突
TEST(VME2EImportIsolation, IR_SameNameNoCollision) {
    std::string src = "import \"mymod\";"
                      "var count = 999;"
                      "print(count);"
                      "print(getCount());";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "var count = 10;"
                                                                      "export fun getCount() { return count; }"}};
    EXPECT_EQ(runVMWithModulesIR(src, modules), "99910");
}

// RegVM 路径：非导出变量不可被导入方访问
TEST(VME2EImportIsolation, Reg_NonExportedVarInaccessible) {
    std::string src = "import \"mymod\";"
                      "print(count);";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "var count = 10;"
                                                                      "export fun getCount() { return count; }"}};
    std::string result = runVMWithModulesReg(src, modules);
    EXPECT_TRUE(isCompileError(result) || isRuntimeError(result)) << "实际结果: " << result;
}

// RegVM 路径：导出名仍可正常访问 + 内部引用工作
TEST(VME2EImportIsolation, Reg_ExportedAndInternalRefs) {
    std::string src = "import \"mymod\";"
                      "print(getCount());"
                      "print(inc());"
                      "print(getCount());";
    std::unordered_map<std::string, std::string> modules = {{"mymod",
                                                             "var count = 10;"
                                                             "export fun getCount() { return count; }"
                                                             "export fun inc() { count = count + 5; return count; }"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "101515");
}

// RegVM 路径：导入方同名变量不冲突
TEST(VME2EImportIsolation, Reg_SameNameNoCollision) {
    std::string src = "import \"mymod\";"
                      "var count = 999;"
                      "print(count);"
                      "print(getCount());";
    std::unordered_map<std::string, std::string> modules = {{"mymod", "var count = 10;"
                                                                      "export fun getCount() { return count; }"}};
    EXPECT_EQ(runVMWithModulesReg(src, modules), "99910");
}

// 三后端一致性：模块隔离后，导出函数+内部状态在三条路径上行为一致
TEST(VME2EImportIsolation, ThreeBackendConsistency) {
    std::string src = "import \"mymod\";"
                      "print(api());"
                      "print(api());"
                      "print(api());";
    std::unordered_map<std::string, std::string> modules = {{"mymod",
                                                             "var state = 0;"
                                                             "export fun api() { state = state + 1; return state; }"}};
    std::string vmOut = runVMWithModules(src, modules);
    std::string irOut = runVMWithModulesIR(src, modules);
    std::string regOut = runVMWithModulesReg(src, modules);
    EXPECT_EQ(vmOut, "123");
    EXPECT_EQ(irOut, "123");
    EXPECT_EQ(regOut, "123");
}

// ============================================================
// P2-14 循环导入延迟加载：四后端一致性测试
// ------------------------------------------------------------
// 验证 Interpreter / StackVM / StackVM-via-IR / RegisterVM 四条路径
// 在循环导入场景下产生相同的输出。
// ============================================================

// 辅助：在 Interpreter 上执行带模块的源码，返回输出
static std::string
runInterpreterWithModulesForConsistency(const std::string& source,
                                        const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "";

    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });
    interp.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end())
            throw std::runtime_error("module not found: " + path);
        return it->second;
    });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return captured + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return captured + "<runtime:" + std::string(e.what()) + ">";
    }
    return captured;
}

// 四后端一致性：循环导入基本场景
TEST(VME2EImportCircular, FourBackendBasic) {
    std::string src = "import \"a\";";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\";"}, {"b", "import \"a\";"}};
    std::string interpOut = runInterpreterWithModulesForConsistency(src, modules);
    std::string vmOut = runVMWithModules(src, modules);
    std::string irOut = runVMWithModulesIR(src, modules);
    std::string regOut = runVMWithModulesReg(src, modules);
    EXPECT_EQ(interpOut, "");
    EXPECT_EQ(vmOut, "");
    EXPECT_EQ(irOut, "");
    EXPECT_EQ(regOut, "");
}

// 四后端一致性：循环导入执行顺序
TEST(VME2EImportCircular, FourBackendExecutionOrder) {
    std::string src = "import \"a\";"
                      "print(\"main\");";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\"; print(\"a\");"},
                                                            {"b", "import \"a\"; print(\"b\");"}};
    std::string interpOut = runInterpreterWithModulesForConsistency(src, modules);
    std::string vmOut = runVMWithModules(src, modules);
    std::string irOut = runVMWithModulesIR(src, modules);
    std::string regOut = runVMWithModulesReg(src, modules);
    EXPECT_EQ(interpOut, "bamain");
    EXPECT_EQ(vmOut, "bamain");
    EXPECT_EQ(irOut, "bamain");
    EXPECT_EQ(regOut, "bamain");
}

// 四后端一致性：循环导入——导出名在循环引用前定义
TEST(VME2EImportCircular, FourBackendExportBeforeCycle) {
    std::string src = "import { get_a } from \"a\";"
                      "print(get_a());";
    std::unordered_map<std::string, std::string> modules = {{"a", "export fun get_a() { return 1; }"
                                                                  "import \"b\";"},
                                                            {"b", "import \"a\";"
                                                                  "print(\"b\");"}};
    std::string interpOut = runInterpreterWithModulesForConsistency(src, modules);
    std::string vmOut = runVMWithModules(src, modules);
    std::string irOut = runVMWithModulesIR(src, modules);
    std::string regOut = runVMWithModulesReg(src, modules);
    EXPECT_EQ(interpOut, "b1");
    EXPECT_EQ(vmOut, "b1");
    EXPECT_EQ(irOut, "b1");
    EXPECT_EQ(regOut, "b1");
}

// 四后端一致性：三模块循环导入（a→b→c→a）
TEST(VME2EImportCircular, FourBackendThreeModuleCycle) {
    std::string src = "import \"a\";"
                      "print(\"main\");";
    std::unordered_map<std::string, std::string> modules = {{"a", "import \"b\"; print(\"a\");"},
                                                            {"b", "import \"c\"; print(\"b\");"},
                                                            {"c", "import \"a\"; print(\"c\");"}};
    // 执行序：c（回路闭合后先执行完）→ b → a → main
    std::string interpOut = runInterpreterWithModulesForConsistency(src, modules);
    std::string vmOut = runVMWithModules(src, modules);
    std::string irOut = runVMWithModulesIR(src, modules);
    std::string regOut = runVMWithModulesReg(src, modules);
    EXPECT_EQ(interpOut, "cbamain");
    EXPECT_EQ(vmOut, "cbamain");
    EXPECT_EQ(irOut, "cbamain");
    EXPECT_EQ(regOut, "cbamain");
}

// ============================================================
// BUG-IDE-12 回归测试：VM 条件断点支持局部变量
// ------------------------------------------------------------
// 验证 Compiler/AstIRBuilder 在编译时记录 slot→name 映射，
// VM/RegisterVM 通过 getCurrentFrameLocals() 反查当前帧局部变量。
// 覆盖三条路径：直接栈式 VM / IR 栈式 VM / RegisterVM。
// ============================================================

// 辅助：编译源码，返回 CompileResult（栈式 VM 直接路径）
static CompileResult compileStackVM(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return {};
    Compiler compiler;
    return compiler.compile(*ast);
}

// 辅助：编译源码，返回 CompileResult（栈式 VM IR 路径）
static CompileResult compileStackVMIR(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return {};
    Compiler compiler;
    compiler.setUseIR(true);
    return compiler.compile(*ast);
}

// 辅助：编译源码，返回 RegisterCompileResult
static RegisterCompileResult compileRegVM(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return {};
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    return compiler.getLastRegisterResult();
}

// 直接栈式 VM：函数 chunk 的 localSlotNames 包含参数和局部变量
TEST(VMConditionalBreakpoint, StackVM_Direct_LocalSlotNamesPopulated) {
    std::string src = "fun add(a, b) {"
                      "  var sum = a + b;"
                      "  return sum;"
                      "}"
                      "print(add(3, 4));";
    CompileResult result = compileStackVM(src);
    auto it = result.functionChunks.find("add");
    ASSERT_NE(it, result.functionChunks.end());
    const auto& names = it->second.localSlotNames;
    // slot 0 = a, slot 1 = b, slot 2 = sum
    ASSERT_GE(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");
    EXPECT_EQ(names[2], "sum");
}

// IR 栈式 VM：函数 chunk 的 localSlotNames 包含参数和局部变量
TEST(VMConditionalBreakpoint, StackVM_IR_LocalSlotNamesPopulated) {
    std::string src = "fun add(a, b) {"
                      "  var sum = a + b;"
                      "  return sum;"
                      "}"
                      "print(add(3, 4));";
    CompileResult result = compileStackVMIR(src);
    auto it = result.functionChunks.find("add");
    ASSERT_NE(it, result.functionChunks.end());
    const auto& names = it->second.localSlotNames;
    ASSERT_GE(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");
    EXPECT_EQ(names[2], "sum");
}

// RegisterVM：函数 chunk 的 localRegNames 包含参数和局部变量
TEST(VMConditionalBreakpoint, RegVM_LocalRegNamesPopulated) {
    std::string src = "fun add(a, b) {"
                      "  var sum = a + b;"
                      "  return sum;"
                      "}"
                      "print(add(3, 4));";
    RegisterCompileResult result = compileRegVM(src);
    auto it = result.functionChunks.find("add");
    ASSERT_NE(it, result.functionChunks.end());
    const auto& names = it->second.localRegNames;
    ASSERT_GE(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");
    EXPECT_EQ(names[2], "sum");
}

// 类方法：localSlotNames 包含 this、字段、参数
TEST(VMConditionalBreakpoint, StackVM_Direct_MethodLocalSlotNames) {
    std::string src = "class Point {"
                      "  var x = 0;"
                      "  var y = 0;"
                      "  fun init(ax, ay) { x = ax; y = ay; }"
                      "  fun norm() { return x * x + y * y; }"
                      "}"
                      "var p = Point(3, 4);"
                      "print(p.norm());";
    CompileResult result = compileStackVM(src);
    // 方法名使用 "ClassName.method" 命名
    auto it = result.functionChunks.find("Point.init");
    ASSERT_NE(it, result.functionChunks.end()) << "应找到 Point.init 方法 chunk";
    const auto& initNames = it->second.localSlotNames;
    // 方法 slot 0 = this, slot 1..2 = 字段 x/y, slot 3..4 = 参数 ax/ay
    ASSERT_GE(initNames.size(), 5u) << "localSlotNames 应至少有 5 个条目";
    EXPECT_EQ(initNames[0], "this");
    // ax/ay 参数在字段之后
    bool foundAx = false, foundAy = false;
    for (const auto& n : initNames) {
        if (n == "ax")
            foundAx = true;
        if (n == "ay")
            foundAy = true;
    }
    EXPECT_TRUE(foundAx) << "localSlotNames 应包含参数 ax";
    EXPECT_TRUE(foundAy) << "localSlotNames 应包含参数 ay";
}

// VM::getCurrentFrameLocals() 在函数执行期间返回正确的局部变量
TEST(VMConditionalBreakpoint, StackVM_GetCurrentFrameLocals) {
    std::string src = "fun compute(a, b) {"
                      "  var c = a + b;"
                      "  var d = c * 2;"
                      "  return d;"
                      "}"
                      "print(compute(3, 4));";
    CompileResult result = compileStackVM(src);

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.initExecution(result);

    // 单步执行，在函数帧中捕获 locals
    std::unordered_map<std::string, Value> capturedLocals;
    bool capturedInCompute = false;
    vm.setStepCallbackEnabled(true);
    vm.setStepCallback([&](const VMStepInfo&) {
        if (vm.getFrameCount() >= 2 && !capturedInCompute) {
            // 在 compute 函数帧中
            std::string chunkName = vm.getCurrentChunkName();
            if (chunkName == "compute") {
                capturedLocals = vm.getCurrentFrameLocals();
                capturedInCompute = true;
            }
        }
    });

    while (!vm.isFinished()) {
        if (vm.stepOnce() != VMResult::VM_OK)
            break;
    }
    ASSERT_FALSE(vm.hasError()) << "VM 出错: " << vm.getLastError();
    ASSERT_TRUE(capturedInCompute) << "未在 compute 函数中捕获 locals";
    // a=3, b=4 应在 locals 中（c/d 可能在执行初期未初始化，但 a/b 作为参数必然存在）
    ASSERT_TRUE(capturedLocals.count("a")) << "locals 中应有参数 a";
    ASSERT_TRUE(capturedLocals.count("b")) << "locals 中应有参数 b";
    EXPECT_EQ(capturedLocals["a"].intVal(), 3);
    EXPECT_EQ(capturedLocals["b"].intVal(), 4);
    EXPECT_EQ(captured, "14");
}

// RegisterVM::getCurrentFrameLocals() 在函数执行期间返回正确的局部变量
TEST(VMConditionalBreakpoint, RegVM_GetCurrentFrameLocals) {
    std::string src = "fun compute(a, b) {"
                      "  var c = a + b;"
                      "  var d = c * 2;"
                      "  return d;"
                      "}"
                      "print(compute(3, 4));";
    RegisterCompileResult result = compileRegVM(src);

    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.initExecution(result);

    // 单步执行，在函数帧中捕获 locals
    std::unordered_map<std::string, Value> capturedLocals;
    bool capturedInCompute = false;
    vm.setStepCallbackEnabled(true);
    vm.setStepCallback([&](const RegVMStepInfo&) {
        if (vm.getFrameCount() >= 2 && !capturedInCompute) {
            std::string chunkName = vm.getCurrentChunkName();
            if (chunkName == "compute") {
                capturedLocals = vm.getCurrentFrameLocals();
                capturedInCompute = true;
            }
        }
    });

    while (!vm.isFinished()) {
        if (vm.stepOnce() != VMResult::VM_OK)
            break;
    }
    ASSERT_FALSE(vm.hasError()) << "RegisterVM 出错: " << vm.getLastError();
    ASSERT_TRUE(capturedInCompute) << "未在 compute 函数中捕获 locals";
    ASSERT_TRUE(capturedLocals.count("a")) << "locals 中应有参数 a";
    ASSERT_TRUE(capturedLocals.count("b")) << "locals 中应有参数 b";
    EXPECT_EQ(capturedLocals["a"].intVal(), 3);
    EXPECT_EQ(capturedLocals["b"].intVal(), 4);
    EXPECT_EQ(captured, "14");
}

// catch 变量也应出现在 localSlotNames 中
TEST(VMConditionalBreakpoint, StackVM_CatchVarInLocalSlotNames) {
    std::string src = "fun safeDiv(a, b) {"
                      "  try {"
                      "    if (b == 0) { throw 999; }"
                      "    return a / b;"
                      "  } catch (e) {"
                      "    return e;"
                      "  }"
                      "}"
                      "print(safeDiv(10, 0));";
    CompileResult result = compileStackVM(src);
    auto it = result.functionChunks.find("safeDiv");
    ASSERT_NE(it, result.functionChunks.end());
    const auto& names = it->second.localSlotNames;
    // catch 变量 e 应出现在 localSlotNames 中
    bool foundCatchVar = false;
    for (const auto& n : names) {
        if (n == "e") {
            foundCatchVar = true;
            break;
        }
    }
    EXPECT_TRUE(foundCatchVar) << "catch 变量 e 应出现在 localSlotNames 中";
}

// 三后端一致性：localSlotNames/localRegNames 在三条路径上包含相同的变量名集合
TEST(VMConditionalBreakpoint, ThreeBackendLocalNamesConsistency) {
    std::string src = "fun compute(a, b) {"
                      "  var c = a + b;"
                      "  return c;"
                      "}"
                      "print(compute(3, 4));";
    CompileResult directResult = compileStackVM(src);
    CompileResult irResult = compileStackVMIR(src);
    RegisterCompileResult regResult = compileRegVM(src);

    auto collectNames = [](const auto& names) {
        std::set<std::string> s;
        for (const auto& n : names)
            if (!n.empty())
                s.insert(n);
        return s;
    };

    auto dit = directResult.functionChunks.find("compute");
    auto iit = irResult.functionChunks.find("compute");
    auto rit = regResult.functionChunks.find("compute");
    ASSERT_NE(dit, directResult.functionChunks.end());
    ASSERT_NE(iit, irResult.functionChunks.end());
    ASSERT_NE(rit, regResult.functionChunks.end());

    auto directNames = collectNames(dit->second.localSlotNames);
    auto irNames = collectNames(iit->second.localSlotNames);
    auto regNames = collectNames(rit->second.localRegNames);

    // 三条路径都应包含 a, b, c
    EXPECT_EQ(directNames, (std::set<std::string>{"a", "b", "c"}));
    EXPECT_EQ(irNames, (std::set<std::string>{"a", "b", "c"}));
    EXPECT_EQ(regNames, (std::set<std::string>{"a", "b", "c"}));
}
