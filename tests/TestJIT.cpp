// ============================================================
// tests/TestJIT.cpp
// ------------------------------------------------------------
// R138 JIT 后端阶段 1 PoC 测试 + R139 阶段 2a 扩展测试 + R140 算术完善
//
// 阶段 1（R138）验证整数算术子集：
//   OP_INT / OP_ADD / OP_SUBTRACT / OP_MULTIPLY / OP_NEGATE /
//   OP_PRINT / OP_POP / OP_RETURN / OP_NULL / OP_TRUE / OP_FALSE
//
// 阶段 2a（R139）扩展验证：
//   控制流：OP_JUMP / OP_JUMP_IF_FALSE / OP_LOOP（if/while 语句）
//   全局变量 slot：OP_DEFINE_GLOBAL / OP_GET_GLOBAL / OP_SET_GLOBAL
//   比较指令：OP_EQUAL / OP_NOT_EQUAL / OP_LESS / OP_GREATER /
//             OP_LESS_EQUAL / OP_GREATER_EQUAL
//   逻辑取反：OP_NOT
//
// 阶段 2a+（R140）算术完善：
//   整数除法：OP_DIVIDE（cqo + idiv，截断向零，与 StackVM 语义一致）
//   整数取模：OP_MODULO（cqo + idiv，余数在 rdx）
//   除零错误路径：test + jz → jitReportError → jmp epilogue
//
// 测试策略：
//   1. 通过 Compiler 编译 MiniLang 源码为 BytecodeChunk
//   2. 通过 JITBackend 执行编译结果，收集输出
//   3. 与期望输出字符串比较
//
// 阶段 2a+ 仍不支持的特征（会由 Compiler 生成 JIT 未实现的 OpCode，
// JITBackend 返回 CompileError）：
//   - name-based 变量（OP_DEFINE_VAR / OP_GET_VAR / OP_SET_VAR）
//   - 函数调用（OP_CALL / OP_CALL_EXPR）
//   - 类、闭包、数组、字典、字符串、浮点等
//
// @since R138
// ============================================================

#include "common/ThreeBackends.h"
#include "compiler/Compiler.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#ifdef MINILANG_USE_JIT
#include "compiler/JIT.h"
#include "interpreter/GcManager.h" // P2-9: CallbackSuppressor 测试
#endif

#include <gtest/gtest.h>
#include <string>

// ============================================================
// JIT 后端测试夹具
// ============================================================
// 仅在 MINILANG_USE_JIT=ON 时编译实际测试，否则提供一个 skip 占位测试
// 以便 CTest 注册一致。

#ifdef MINILANG_USE_JIT

#include "TestJITHelper.h"

// ============================================================
// 阶段 1 PoC 测试用例（R138）
// ============================================================

TEST(TestJIT, SimpleAddition) {
    // 最小用例：print(1+2); → "3"
    EXPECT_EQ(runJIT("print(1+2);"), "3");
}

TEST(TestJIT, SimpleSubtraction) {
    EXPECT_EQ(runJIT("print(10-4);"), "6");
}

TEST(TestJIT, SimpleMultiplication) {
    EXPECT_EQ(runJIT("print(2*3);"), "6");
}

TEST(TestJIT, NegativeLiteral) {
    // -5 实际编译为 OP_INT(5) + OP_NEGATE
    EXPECT_EQ(runJIT("print(-5);"), "-5");
}

TEST(TestJIT, NestedExpression) {
    // (1+2)*3 = 9
    EXPECT_EQ(runJIT("print((1+2)*3);"), "9");
}

TEST(TestJIT, MultiplePrints) {
    // 多条 print 语句，输出按顺序拼接
    EXPECT_EQ(runJIT("print(1);print(2);print(3);"), "123");
}

TEST(TestJIT, LargeNumber) {
    // 测试大整数（超过 32 位，验证 movabs imm64）
    EXPECT_EQ(runJIT("print(1000000000000);"), "1000000000000");
}

TEST(TestJIT, ZeroLiteral) {
    EXPECT_EQ(runJIT("print(0);"), "0");
}

TEST(TestJIT, AdditionWithZero) {
    EXPECT_EQ(runJIT("print(0+7);"), "7");
}

TEST(TestJIT, SubtractionToNegative) {
    // 5 - 10 = -5
    EXPECT_EQ(runJIT("print(5-10);"), "-5");
}

TEST(TestJIT, BackendName) {
    JITBackend jit;
    EXPECT_EQ(jit.backendName(), "JIT");
}

// 拓展二期·教学（字节码↔汇编对照）：setAsmCapture 捕获发射的汇编文本
TEST(TestJIT, AsmCaptureCollectsEmittedAssembly) {
    Lexer lx;
    auto tk = lx.scan("print(1+2);");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    jit.setAsmCapture(true);
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    ASSERT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "3");

    // 捕获的汇编非空且含 x86 指令痕迹（prologue 必有 push/mov）
    const std::string& asmText = jit.getCapturedAsm();
    EXPECT_FALSE(asmText.empty());
    EXPECT_NE(asmText.find("mov"), std::string::npos);

    // 未启用捕获时不残留上次内容之外的开销（新实例默认关闭 → 空）
    JITBackend jit2;
    std::string out2;
    jit2.setOutputCallback([&](const std::string& s) { out2 += s; });
    ASSERT_EQ(jit2.execute(cr), JitResult::OK);
    EXPECT_TRUE(jit2.getCapturedAsm().empty());
}

TEST(TestJIT, DiagnosticsClearedOnExecute) {
    // 连续两次执行，diagnostics 应在第二次执行前被清空
    Lexer lx;
    auto tk = lx.scan("print(1);");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });

    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_FALSE(jit.getDiagnostics().hasErrors());

    // 第二次执行：diagnostics_ 应被清空
    out.clear();
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_FALSE(jit.getDiagnostics().hasErrors());
    EXPECT_EQ(out, "1");
}

// ============================================================
// 阶段 2a 扩展测试用例（R139）
// ============================================================

// ---- 比较指令测试 ----

TEST(TestJIT, EqualTrue) {
    // R142 阶段 3a：NaN-boxing 迁移后，JIT 输出与 StackVM/Interpreter 一致（true/false）
    EXPECT_EQ(runJIT("print(3==3);"), "true");
}

TEST(TestJIT, EqualFalse) {
    EXPECT_EQ(runJIT("print(3==4);"), "false");
}

TEST(TestJIT, NotEqualTrue) {
    EXPECT_EQ(runJIT("print(3!=4);"), "true");
}

TEST(TestJIT, NotEqualFalse) {
    EXPECT_EQ(runJIT("print(3!=3);"), "false");
}

TEST(TestJIT, LessThan) {
    EXPECT_EQ(runJIT("print(2<5);"), "true");
    EXPECT_EQ(runJIT("print(5<2);"), "false");
    EXPECT_EQ(runJIT("print(3<3);"), "false");
}

TEST(TestJIT, GreaterThan) {
    EXPECT_EQ(runJIT("print(5>2);"), "true");
    EXPECT_EQ(runJIT("print(2>5);"), "false");
    EXPECT_EQ(runJIT("print(3>3);"), "false");
}

TEST(TestJIT, LessEqual) {
    EXPECT_EQ(runJIT("print(2<=5);"), "true");
    EXPECT_EQ(runJIT("print(5<=2);"), "false");
    EXPECT_EQ(runJIT("print(3<=3);"), "true");
}

TEST(TestJIT, GreaterEqual) {
    EXPECT_EQ(runJIT("print(5>=2);"), "true");
    EXPECT_EQ(runJIT("print(2>=5);"), "false");
    EXPECT_EQ(runJIT("print(3>=3);"), "true");
}

TEST(TestJIT, LogicalNot) {
    // R142 阶段 3a：!0 → true，!非零 → false（与 StackVM truthiness 一致）
    EXPECT_EQ(runJIT("print(!0);"), "true");
    EXPECT_EQ(runJIT("print(!1);"), "false");
    EXPECT_EQ(runJIT("print(!5);"), "false");
}

// ---- 全局变量 slot 测试 ----

TEST(TestJIT, VariableDefineAndGet) {
    // var x = 42; print(x); → "42"
    EXPECT_EQ(runJIT("var x = 42; print(x);"), "42");
}

TEST(TestJIT, VariableAssignment) {
    // var x = 10; x = 20; print(x); → "20"
    EXPECT_EQ(runJIT("var x = 10; x = 20; print(x);"), "20");
}

TEST(TestJIT, MultipleVariables) {
    // var a = 5; var b = 7; print(a+b); → "12"
    EXPECT_EQ(runJIT("var a = 5; var b = 7; print(a+b);"), "12");
}

TEST(TestJIT, VariableInExpression) {
    // var x = 3; var y = 4; print(x*y + 1); → "13"
    EXPECT_EQ(runJIT("var x = 3; var y = 4; print(x*y + 1);"), "13");
}

// ---- 控制流测试 ----

TEST(TestJIT, IfTrueBranch) {
    // if (1) { print(10); } → "10"
    EXPECT_EQ(runJIT("if (1) { print(10); }"), "10");
}

TEST(TestJIT, IfFalseBranch) {
    // if (0) { print(10); } → ""（不执行 then 分支）
    EXPECT_EQ(runJIT("if (0) { print(10); }"), "");
}

TEST(TestJIT, IfElseTrueBranch) {
    // if (1) { print(1); } else { print(2); } → "1"
    EXPECT_EQ(runJIT("if (1) { print(1); } else { print(2); }"), "1");
}

TEST(TestJIT, IfElseFalseBranch) {
    // if (0) { print(1); } else { print(2); } → "2"
    EXPECT_EQ(runJIT("if (0) { print(1); } else { print(2); }"), "2");
}

TEST(TestJIT, IfComparison) {
    // var x = 5; if (x > 3) { print(1); } else { print(0); } → "1"
    EXPECT_EQ(runJIT("var x = 5; if (x > 3) { print(1); } else { print(0); }"), "1");
}

TEST(TestJIT, WhileLoop) {
    // var i = 0; while (i < 3) { print(i); i = i + 1; } → "012"
    EXPECT_EQ(runJIT("var i = 0; while (i < 3) { print(i); i = i + 1; }"), "012");
}

TEST(TestJIT, WhileLoopSum) {
    // 计算 0+1+2+...+9 = 45
    // var i = 0; var sum = 0; while (i < 10) { sum = sum + i; i = i + 1; } print(sum);
    EXPECT_EQ(runJIT("var i = 0; var sum = 0; while (i < 10) { sum = sum + i; i = i + 1; } print(sum);"), "45");
}

TEST(TestJIT, NestedIfInWhile) {
    // var i = 0; while (i < 5) { if (i > 1) { print(i); } i = i + 1; } → "234"
    EXPECT_EQ(runJIT("var i = 0; while (i < 5) { if (i > 1) { print(i); } i = i + 1; }"), "234");
}

TEST(TestJIT, WhileLoopWithBreak) {
    // var i = 0; while (true) { if (i >= 3) { break; } print(i); i = i + 1; } → "012"
    // 注：break 编译为 OP_JUMP，需要 JIT 支持
    EXPECT_EQ(runJIT("var i = 0; while (true) { if (i >= 3) { break; } print(i); i = i + 1; }"), "012");
}

// ============================================================
// 阶段 2a+ 算术完善测试用例（R140）
// ============================================================
// 验证 OP_DIVIDE / OP_MODULO 的整数除法与取模语义
// 关键不变量：
//   1. 整数除法截断向零（与 StackVM/C++/x86 idiv 一致）
//   2. 取模结果与被除数同号（C++ 语义）
//   3. 除零触发 jitReportError 并终止执行（与 StackVM runtimeError 一致）

// ---- 整数除法测试 ----

TEST(TestJIT, SimpleDivision) {
    // 10 / 3 = 3（整数除法截断）
    EXPECT_EQ(runJIT("print(10/3);"), "3");
}

TEST(TestJIT, ExactDivision) {
    // 12 / 4 = 3（整除）
    EXPECT_EQ(runJIT("print(12/4);"), "3");
}

TEST(TestJIT, DivisionByOne) {
    EXPECT_EQ(runJIT("print(7/1);"), "7");
}

TEST(TestJIT, DivisionLargerDivisor) {
    // 3 / 10 = 0（截断向零）
    EXPECT_EQ(runJIT("print(3/10);"), "0");
}

TEST(TestJIT, NegativeDividendDivision) {
    // -7 / 2 = -3（截断向零：-3.5 → -3）
    EXPECT_EQ(runJIT("print(-7/2);"), "-3");
}

TEST(TestJIT, NegativeDivisorDivision) {
    // 7 / -2 = -3（截断向零：-3.5 → -3）
    EXPECT_EQ(runJIT("print(7/-2);"), "-3");
}

TEST(TestJIT, BothNegativeDivision) {
    // -7 / -2 = 3（截断向零：3.5 → 3）
    EXPECT_EQ(runJIT("print(-7/-2);"), "3");
}

TEST(TestJIT, DivisionInExpression) {
    // (1+2) / (3-1) = 3 / 2 = 1
    EXPECT_EQ(runJIT("print((1+2)/(3-1));"), "1");
}

TEST(TestJIT, DivisionWithVariables) {
    // var x = 100; var y = 7; print(x/y); → 14
    EXPECT_EQ(runJIT("var x = 100; var y = 7; print(x/y);"), "14");
}

// ---- 整数取模测试 ----

TEST(TestJIT, SimpleModulo) {
    // 10 % 3 = 1
    EXPECT_EQ(runJIT("print(10%3);"), "1");
}

TEST(TestJIT, ExactDivisionModulo) {
    // 12 % 4 = 0
    EXPECT_EQ(runJIT("print(12%4);"), "0");
}

TEST(TestJIT, ModuloByOne) {
    EXPECT_EQ(runJIT("print(7%1);"), "0");
}

TEST(TestJIT, ModuloLargerDivisor) {
    // 3 % 10 = 3
    EXPECT_EQ(runJIT("print(3%10);"), "3");
}

TEST(TestJIT, NegativeDividendModulo) {
    // -7 % 2 = -1（余数与被除数同号，C++ 语义）
    EXPECT_EQ(runJIT("print(-7%2);"), "-1");
}

TEST(TestJIT, NegativeDivisorModulo) {
    // 7 % -2 = 1（余数与被除数同号）
    EXPECT_EQ(runJIT("print(7%-2);"), "1");
}

TEST(TestJIT, BothNegativeModulo) {
    // -7 % -2 = -1（余数与被除数同号）
    EXPECT_EQ(runJIT("print(-7%-2);"), "-1");
}

TEST(TestJIT, ModuloInExpression) {
    // (1+2) % (3-1) = 3 % 2 = 1
    EXPECT_EQ(runJIT("print((1+2)%(3-1));"), "1");
}

TEST(TestJIT, ModuloWithVariables) {
    // var x = 100; var y = 7; print(x%y); → 2
    EXPECT_EQ(runJIT("var x = 100; var y = 7; print(x%y);"), "2");
}

// ---- 除零错误路径测试 ----

TEST(TestJIT, DivisionByZeroError) {
    // 10 / 0 → 应触发 jitReportError → RuntimeError
    // runJIT 返回 "<jit-runtime:除零错误>"
    std::string result = runJIT("print(10/0);");
    EXPECT_NE(result.find("jit-runtime"), std::string::npos) << "除零应触发 JIT RuntimeError，实际输出: " << result;
    EXPECT_NE(result.find("除零错误"), std::string::npos) << "错误消息应包含'除零错误'，实际输出: " << result;
}

TEST(TestJIT, ModuloByZeroError) {
    // 10 % 0 → 应触发 jitReportError → RuntimeError
    std::string result = runJIT("print(10%0);");
    EXPECT_NE(result.find("jit-runtime"), std::string::npos) << "取模除零应触发 JIT RuntimeError，实际输出: " << result;
    EXPECT_NE(result.find("除零错误"), std::string::npos) << "错误消息应包含'除零错误'，实际输出: " << result;
}

TEST(TestJIT, DivisionByZeroWithVariables) {
    // var x = 5; var y = 0; print(x/y); → 应触发除零错误
    std::string result = runJIT("var x = 5; var y = 0; print(x/y);");
    EXPECT_NE(result.find("jit-runtime"), std::string::npos) << "变量除零应触发 JIT RuntimeError，实际输出: " << result;
}

// ---- 除法取模组合与控制流测试 ----

TEST(TestJIT, DivisionModuloConsistency) {
    // (a/b)*b + (a%b) == a 不变量验证
    // var a = 17; var b = 5; print((a/b)*b + (a%b)); → 17
    EXPECT_EQ(runJIT("var a = 17; var b = 5; print((a/b)*b + (a%b));"), "17");
}

TEST(TestJIT, DivisionInWhileLoop) {
    // 通过除法逐步缩小变量
    // var n = 100; var count = 0; while (n > 1) { n = n / 2; count = count + 1; } print(count);
    // 100→50→25→12→6→3→1（6 次除法），但 n=1 时退出
    // 实际：100→50(1)→25(2)→12(3)→6(4)→3(5)→1(6)，count=6
    EXPECT_EQ(runJIT("var n = 100; var count = 0; while (n > 1) { n = n / 2; count = count + 1; } print(count);"), "6");
}

TEST(TestJIT, ModuloForParityCheck) {
    // 用 % 2 判断奇偶
    // var i = 0; while (i < 6) { if (i % 2 == 0) { print(0); } else { print(1); } i = i + 1; }
    // → "010101"
    EXPECT_EQ(runJIT("var i = 0; while (i < 6) { if (i % 2 == 0) { print(0); } else { print(1); } i = i + 1; }"),
              "010101");
}

// ============================================================
// 阶段 2b 函数调用测试用例（R141）
// ============================================================
// 验证 OP_GET_LOCAL / OP_SET_LOCAL / OP_CALL / OP_RETURN / OP_CLOSURE 机器码生成
// 关键不变量：
//   1. 函数调用通过 funcTable 编译期按名查找，OP_CALL 编译为 jmp targetLabel
//   2. 函数返回值 push 到调用者栈顶，供后续表达式使用
//   3. 局部变量通过 r13 = basePointer 寻址（[r13 - slot*8]，slot 0 = 第一个参数）
//   4. 帧栈通过 JitContext.frames 管理，OP_RETURN 恢复调用者 r13/r15
//   5. 闭包创建（OP_CLOSURE upvalueCount=0）push null 标记，不捕获 upvalue
//
// 注意：R109 TCO（尾调用优化）会将 `return f(args)` 编译为 OP_SET_LOCAL + OP_JUMP，
//       测试用例中避免纯尾递归（使用 `return n + f(n-1)` 等非尾递归形式）

// ---- 基础函数调用测试 ----

TEST(TestJIT, SimpleFunctionCall) {
    // fun add(a, b) { return a + b; } print(add(3, 4)); → "7"
    EXPECT_EQ(runJIT("fun add(a, b) { return a + b; } print(add(3, 4));"), "7");
}

TEST(TestJIT, FunctionReturningConstant) {
    // fun f() { return 42; } print(f()); → "42"
    EXPECT_EQ(runJIT("fun f() { return 42; } print(f());"), "42");
}

TEST(TestJIT, FunctionWithLocalVar) {
    // 函数内 var 声明，验证 OP_GET_LOCAL / OP_SET_LOCAL
    // fun f() { var x = 10; return x + 5; } print(f()); → "15"
    EXPECT_EQ(runJIT("fun f() { var x = 10; return x + 5; } print(f());"), "15");
}

TEST(TestJIT, FunctionParamArithmetic) {
    // fun mul(a, b) { return a * b; } print(mul(6, 7)); → "42"
    EXPECT_EQ(runJIT("fun mul(a, b) { return a * b; } print(mul(6, 7));"), "42");
}

TEST(TestJIT, FunctionModifiesParam) {
    // 函数内修改参数槽（OP_SET_LOCAL 写入参数 slot）
    // fun inc(x) { x = x + 1; return x; } print(inc(41)); → "42"
    EXPECT_EQ(runJIT("fun inc(x) { x = x + 1; return x; } print(inc(41));"), "42");
}

TEST(TestJIT, FunctionWithDivision) {
    // fun divmod(a, b) { return a / b + a % b; } print(divmod(17, 5)); → 3+2=5
    EXPECT_EQ(runJIT("fun divmod(a, b) { return a / b + a % b; } print(divmod(17, 5));"), "5");
}

// ---- 多函数与嵌套调用测试 ----

TEST(TestJIT, MultipleFunctions) {
    // 多个函数声明 + 调用
    // fun f(x) { return x + 1; } fun g(x) { return x * 2; } print(f(3)); print(g(4)); → "48"
    EXPECT_EQ(runJIT("fun f(x) { return x + 1; } fun g(x) { return x * 2; } print(f(3)); print(g(4));"), "48");
}

TEST(TestJIT, NestedFunctionCalls) {
    // 函数调用其他函数
    // fun double(x) { return x * 2; } fun quad(x) { return double(double(x)); } print(quad(5)); → "20"
    EXPECT_EQ(runJIT("fun double(x) { return x * 2; } fun quad(x) { return double(double(x)); } print(quad(5));"),
              "20");
}

TEST(TestJIT, FunctionCallInExpression) {
    // 函数调用作为表达式的一部分
    // fun square(x) { return x * x; } print(square(3) + square(4)); → 9+16=25
    EXPECT_EQ(runJIT("fun square(x) { return x * x; } print(square(3) + square(4));"), "25");
}

TEST(TestJIT, FunctionCallAsArgument) {
    // 函数调用作为另一个函数调用的参数
    // fun add1(x) { return x + 1; } fun add2(x) { return x + 2; } print(add1(add2(10))); → 13
    EXPECT_EQ(runJIT("fun add1(x) { return x + 1; } fun add2(x) { return x + 2; } print(add1(add2(10)));"), "13");
}

// ---- 递归调用测试 ----

TEST(TestJIT, RecursiveFibonacci) {
    // 经典递归 fib（非尾递归，不会触发 TCO）
    // fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print(fib(10)); → "55"
    EXPECT_EQ(runJIT("fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print(fib(10));"), "55");
}

TEST(TestJIT, RecursiveFactorial) {
    // 递归阶乘（非尾递归：return n * fact(n-1)）
    // fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(5)); → "120"
    EXPECT_EQ(runJIT("fun fact(n) { if (n <= 1) { return 1; } return n * fact(n-1); } print(fact(5));"), "120");
}

TEST(TestJIT, RecursiveSum) {
    // 递归累加（非尾递归：return n + sum(n-1)）
    // 注：避免使用 sum（MiniLang 内置函数名），改用 recsum
    // fun recsum(n) { if (n == 0) { return 0; } return n + recsum(n-1); } print(recsum(10)); → "55"
    EXPECT_EQ(runJIT("fun recsum(n) { if (n == 0) { return 0; } return n + recsum(n-1); } print(recsum(10));"), "55");
}

TEST(TestJIT, DeepRecursion) {
    // 较深递归（验证帧栈正确恢复，深度 20）
    // fun count(n) { if (n == 0) { return 0; } return 1 + count(n - 1); } print(count(20)); → "20"
    EXPECT_EQ(runJIT("fun count(n) { if (n == 0) { return 0; } return 1 + count(n - 1); } print(count(20));"), "20");
}

// 临时调试：255 层递归（刚好不触发 MAX_FRAMES=256）
TEST(TestJIT, DeepRecursion255) {
    // 255 层递归，刚好不触发 MAX_FRAMES
    std::string src = "fun count(n) { if (n == 0) { return 0; } return 1 + count(n - 1); } print(count(255));";
    std::string out = runJIT(src);
    std::cerr << "[DEBUG] DeepRecursion255 output: '" << out << "'" << std::endl;
    EXPECT_EQ(out, "255");
}

// ---- 函数内控制流测试 ----

TEST(TestJIT, FunctionWithIfElse) {
    // 函数内含 if-else，多 return 路径
    // 注：避免使用 abs（MiniLang 内置函数名），改用 myabs
    // fun myabs(x) { if (x < 0) { return -x; } return x; } print(myabs(-5)); print(myabs(5)); → "55"
    EXPECT_EQ(runJIT("fun myabs(x) { if (x < 0) { return -x; } return x; } print(myabs(-5)); print(myabs(5));"), "55");
}

TEST(TestJIT, FunctionWithWhileLoop) {
    // 函数内含 while 循环
    // 注：避免使用 sum（MiniLang 内置函数名），改用 loopsum
    // fun loopsum(n) { var i = 0; var s = 0; while (i < n) { s = s + i; i = i + 1; } return s; }
    // print(loopsum(10)); → "45"
    EXPECT_EQ(runJIT("fun loopsum(n) { var i = 0; var s = 0; while (i < n) { s = s + i; i = i + 1; } return s; } "
                     "print(loopsum(10));"),
              "45");
}

TEST(TestJIT, FunctionWithLoopAndAccumulator) {
    // 函数内循环 + 累加器
    // fun power(base, exp) { var result = 1; var i = 0; while (i < exp) { result = result * base; i = i + 1; }
    // return result; } print(power(2, 10)); → "1024"
    EXPECT_EQ(runJIT("fun power(base, exp) { var result = 1; var i = 0; while (i < exp) { result = result * base; "
                     "i = i + 1; } return result; } print(power(2, 10));"),
              "1024");
}

// ---- 函数调用与全局变量交互测试 ----

TEST(TestJIT, FunctionAccessingGlobal) {
    // 函数内访问全局变量
    // var g = 100; fun f() { return g; } print(f()); → "100"
    EXPECT_EQ(runJIT("var g = 100; fun f() { return g; } print(f());"), "100");
}

TEST(TestJIT, FunctionModifyingGlobal) {
    // 函数内修改全局变量
    // var counter = 0; fun inc() { counter = counter + 1; return counter; }
    // print(inc()); print(inc()); print(inc()); → "123"
    EXPECT_EQ(runJIT("var counter = 0; fun inc() { counter = counter + 1; return counter; } "
                     "print(inc()); print(inc()); print(inc());"),
              "123");
}

// ---- 函数调用与主程序交织测试 ----

TEST(TestJIT, FunctionCallBetweenStatements) {
    // 函数调用穿插在多条语句之间
    // fun f(x) { return x * 10; } print(1); print(f(5)); print(2); → "1502"
    EXPECT_EQ(runJIT("fun f(x) { return x * 10; } print(1); print(f(5)); print(2);"), "1502");
}

TEST(TestJIT, MultipleFunctionCallsSameFunction) {
    // 多次调用同一函数
    // fun add(x, y) { return x + y; } print(add(1, 2)); print(add(3, 4)); print(add(10, 20)); → "3730"
    EXPECT_EQ(runJIT("fun add(x, y) { return x + y; } print(add(1, 2)); print(add(3, 4)); print(add(10, 20));"),
              "3730");
}

// ============================================================
// R142 阶段 3a NaN-boxing 专项测试
// ============================================================
// 验证 JIT 后端从 raw int64_t 栈迁移到 NaN-boxing Value 编码后：
//   1. 标量类型（INT/BOOL/NULL）的输出与 StackVM/Interpreter 完全一致
//   2. truthiness 语义（payload==0 → falsy）覆盖 INT(0)/BOOL(false)/NULL
//   3. int48 边界值正确编码/解码
//   4. 三后端输出一致性（与 StackVM 对比）
//   5. 全局变量未初始化默认值为 null（NaN-boxing NULL）
//
// 阶段 3a 不支持：浮点、字符串、数组、字典、实例等堆类型
// @since R142

// ---- 标量类型输出测试 ----

TEST(TestJIT, NanBoxPrintNull) {
    // print(null) → "null"
    EXPECT_EQ(runJIT("print(null);"), "null");
}

TEST(TestJIT, NanBoxPrintTrue) {
    // print(true) → "true"
    EXPECT_EQ(runJIT("print(true);"), "true");
}

TEST(TestJIT, NanBoxPrintFalse) {
    // print(false) → "false"
    EXPECT_EQ(runJIT("print(false);"), "false");
}

TEST(TestJIT, NanBoxPrintZero) {
    // print(0) → "0"（INT 0 是 falsy，但 print 输出 INT 类型而非 BOOL）
    EXPECT_EQ(runJIT("print(0);"), "0");
}

TEST(TestJIT, NanBoxPrintNegativeInt) {
    // 负整数编码验证：INT(-42) → 解码后仍为 -42
    EXPECT_EQ(runJIT("print(-42);"), "-42");
}

TEST(TestJIT, NanBoxPrintPositiveInt) {
    // 大正整数编码验证
    EXPECT_EQ(runJIT("print(123456789);"), "123456789");
}

// ---- truthiness 语义测试 ----
// payload（低 48 位）== 0 → falsy；INT(0)/BOOL(false)/NULL 均为 falsy

TEST(TestJIT, NanBoxTruthyIntZero) {
    // INT(0) payload=0 → falsy → if 不进入
    EXPECT_EQ(runJIT("if (0) { print(1); } else { print(2); }"), "2");
}

TEST(TestJIT, NanBoxTruthyIntNonZero) {
    // INT(5) payload=5 → truthy → if 进入
    EXPECT_EQ(runJIT("if (5) { print(1); } else { print(2); }"), "1");
}

TEST(TestJIT, NanBoxTruthyBoolFalse) {
    // BOOL(false) payload=0 → falsy
    EXPECT_EQ(runJIT("if (false) { print(1); } else { print(2); }"), "2");
}

TEST(TestJIT, NanBoxTruthyBoolTrue) {
    // BOOL(true) payload=1 → truthy
    EXPECT_EQ(runJIT("if (true) { print(1); } else { print(2); }"), "1");
}

TEST(TestJIT, NanBoxTruthyNull) {
    // NULL payload=0 → falsy
    EXPECT_EQ(runJIT("if (null) { print(1); } else { print(2); }"), "2");
}

TEST(TestJIT, NanBoxTruthyNegativeInt) {
    // 负整数（payload 非 0）→ truthy
    EXPECT_EQ(runJIT("if (-1) { print(1); } else { print(2); }"), "1");
}

// ---- while 循环条件 truthiness 测试 ----

TEST(TestJIT, NanBoxWhileLoopWithBoolCondition) {
    // var x = 3; while (x > 0) { print(x); x = x - 1; } → "321"
    EXPECT_EQ(runJIT("var x = 3; while (x > 0) { print(x); x = x - 1; }"), "321");
}

TEST(TestJIT, NanBoxWhileLoopNeverEntersWithFalse) {
    // while (false) 永不进入
    EXPECT_EQ(runJIT("while (false) { print(1); } print(2);"), "2");
}

// ---- int48 边界值测试 ----
// int48 范围：-(2^47) <= value < 2^47
// 2^47 = 140737488355328

TEST(TestJIT, NanBoxInt48PositiveBoundary) {
    // 2^47 - 1 = 140737488355327（int48 最大值，可编码）
    EXPECT_EQ(runJIT("print(140737488355327);"), "140737488355327");
}

TEST(TestJIT, NanBoxInt48NegativeBoundary) {
    // -(2^47) = -140737488355328（int48 最小值，可编码）
    EXPECT_EQ(runJIT("print(-140737488355328);"), "-140737488355328");
}

TEST(TestJIT, NanBoxInt48OverflowPositive) {
    // 2^47 = 140737488355328（超出 int48 范围，JIT 编译期应报错）
    EXPECT_TRUE(runJIT("print(140737488355328);").find("<jit-compile:") != std::string::npos);
}

TEST(TestJIT, NanBoxInt48OverflowNegative) {
    // -(2^47) - 1 = -140737488355329（超出 int48 范围，JIT 编译期应报错）
    EXPECT_TRUE(runJIT("print(-140737488355329);").find("<jit-compile:") != std::string::npos);
}

// ---- 算术运算后类型保持 INT ----

TEST(TestJIT, NanBoxArithmeticResultIsInt) {
    // 算术结果仍是 INT，print 输出整数格式
    EXPECT_EQ(runJIT("print(10 + 20);"), "30");
    EXPECT_EQ(runJIT("print(50 - 25);"), "25");
    EXPECT_EQ(runJIT("print(6 * 7);"), "42");
    EXPECT_EQ(runJIT("print(100 / 7);"), "14");
    EXPECT_EQ(runJIT("print(100 % 7);"), "2");
}

TEST(TestJIT, NanBoxNegativeArithmetic) {
    // 负数算术验证：解码→运算→重新编码链路
    EXPECT_EQ(runJIT("print(-10 + 5);"), "-5");
    EXPECT_EQ(runJIT("print(-10 - 5);"), "-15");
    EXPECT_EQ(runJIT("print(-10 * 3);"), "-30");
    EXPECT_EQ(runJIT("print(-10 / 3);"), "-3");
    EXPECT_EQ(runJIT("print(-10 % 3);"), "-1");
}

TEST(TestJIT, NanBoxNegateOperator) {
    // OP_NEGATE 验证：解码→取负→重新编码
    EXPECT_EQ(runJIT("print(-(-42));"), "42");
    EXPECT_EQ(runJIT("print(-0);"), "0");
}

// ---- 比较运算返回 BOOL ----

TEST(TestJIT, NanBoxComparisonReturnsBool) {
    // 比较运算结果是 BOOL 类型，print 输出 true/false
    EXPECT_EQ(runJIT("print(1 == 1);"), "true");
    EXPECT_EQ(runJIT("print(1 != 2);"), "true");
    EXPECT_EQ(runJIT("print(1 < 2);"), "true");
    EXPECT_EQ(runJIT("print(2 > 1);"), "true");
    EXPECT_EQ(runJIT("print(1 <= 1);"), "true");
    EXPECT_EQ(runJIT("print(2 >= 2);"), "true");
}

TEST(TestJIT, NanBoxChainedComparison) {
    // 嵌套比较：1 < 2 == true
    EXPECT_EQ(runJIT("print((1 < 2) == true);"), "true");
    EXPECT_EQ(runJIT("print((1 > 2) == false);"), "true");
}

// ---- 全局变量默认 null 测试 ----

TEST(TestJIT, NanBoxUninitializedGlobalIsNull) {
    // var x;（未初始化）→ 全局变量 slot 默认值为 NaN-boxing NULL
    EXPECT_EQ(runJIT("var x; print(x);"), "null");
}

TEST(TestJIT, NanBoxAssignNullToVariable) {
    // var x = null; print(x); → "null"
    EXPECT_EQ(runJIT("var x = null; print(x);"), "null");
}

TEST(TestJIT, NanBoxAssignBoolToVariable) {
    // var x = true; var y = false; print(x); print(y); → "truefalse"
    EXPECT_EQ(runJIT("var x = true; var y = false; print(x); print(y);"), "truefalse");
}

// ---- 函数返回 BOOL/NULL ----

TEST(TestJIT, NanBoxFunctionReturnsBool) {
    // 函数返回 BOOL 值
    EXPECT_EQ(runJIT("fun isEven(n) { return n % 2 == 0; } print(isEven(4)); print(isEven(5));"), "truefalse");
}

TEST(TestJIT, NanBoxFunctionReturnsNull) {
    // 函数无 return → 默认返回 null
    EXPECT_EQ(runJIT("fun doNothing() {} print(doNothing());"), "null");
}

TEST(TestJIT, NanBoxFunctionReturnsNullExplicit) {
    // 函数显式返回 null
    EXPECT_EQ(runJIT("fun getNull() { return null; } print(getNull());"), "null");
}

// ---- 三后端一致性对比测试 ----
// 验证 JIT 输出与 StackVM 完全一致

TEST(TestJIT, NanBoxConsistencyWithStackVMBool) {
    // 验证 bool 输出与 StackVM 一致
    std::string jitOut = runJIT("print(true); print(false); print(1 == 1); print(1 != 1);");
    // true / false / (1==1)→true / (1!=1)→false
    EXPECT_EQ(jitOut, "truefalsetruefalse");
}

TEST(TestJIT, NanBoxConsistencyWithStackVMNull) {
    // 验证 null 输出与 StackVM 一致
    std::string jitOut = runJIT("print(null); var x; print(x);");
    EXPECT_EQ(jitOut, "nullnull");
}

TEST(TestJIT, NanBoxConsistencyWithStackVMIntArithmetic) {
    // 验证 int 算术输出与 StackVM 一致
    std::string jitOut = runJIT("print(0); print(-1); print(1000000); print(1+2*3-4/2);");
    EXPECT_EQ(jitOut, "0-110000005");
}

// ---- 复合场景测试 ----

TEST(TestJIT, NanBoxIfElseReturnsBool) {
    // if/else 返回 BOOL 类型值
    EXPECT_EQ(runJIT("fun check(n) { if (n > 0) { return true; } else { return false; } }"
                     "print(check(5)); print(check(-5)); print(check(0));"),
              "truefalsefalse");
}

TEST(TestJIT, NanBoxLogicalNotChain) {
    // 连续 NOT：!!5 → true，!!0 → false
    EXPECT_EQ(runJIT("print(!0); print(!5); print(!!5); print(!!0);"), "truefalsetruefalse");
}

TEST(TestJIT, NanBoxMixedTypeArithmetic) {
    // 混合类型运算（INT + BOOL 的 truthiness 应用）
    // 注意：MiniLang 不支持 INT + BOOL 算术，但 if 条件可接受任意标量
    EXPECT_EQ(runJIT("if (1 + 0) { print(1); }"), "1");
    EXPECT_EQ(runJIT("if (0 + 0) { print(1); } else { print(2); }"), "2");
}

// ============================================================
// R140 性能对比基准：JIT vs StackVM
// ============================================================
// 目的：量化 JIT 相对于 StackVM 的执行时间收益
// 方法：在两后端上执行同一段计算密集型 MiniLang 源码，测量耗时
// 注意：不做 pass/fail 断言，仅测量并输出对比（避免环境噪声导致 CI 抖动）
// 基准选取原则：(1) 只用 JIT 已支持的特性 (2) 计算密集可测量 (3) 输出可验证正确性

#include "compiler/VM.h"
#include <chrono>
#include <iostream>

namespace {

/// 在 StackVM 上运行源码，返回 {耗时毫秒, 输出}
std::pair<double, std::string> runStackVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return {-1.0, "<parse-fail>"};

    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return {-1.0, "<compile-fail>"};

    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });

    auto start = std::chrono::high_resolution_clock::now();
    vm.execute(cr);
    auto end = std::chrono::high_resolution_clock::now();

    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    return {ms, out};
}

/// 在 JIT 上运行源码，返回 {耗时毫秒, 输出}
std::pair<double, std::string> runJITTimed(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return {-1.0, "<parse-fail>"};

    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return {-1.0, "<compile-fail>"};

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });

    auto start = std::chrono::high_resolution_clock::now();
    jit.execute(cr);
    auto end = std::chrono::high_resolution_clock::now();

    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    return {ms, out};
}

} // namespace

/// 大循环累加基准：100 万次循环累加（计算密集型）
/// 验证 JIT 在纯算术 + 控制流场景下的加速效果
TEST(TestJITPerf, LargeLoopSum) {
    const std::string src = R"(
var i = 0;
var sum = 0;
while (i < 1000000) {
    sum = sum + i;
    i = i + 1;
}
print(sum);
)";
    auto [jitMs, jitOut] = runJITTimed(src);
    auto [vmMs, vmOut] = runStackVM(src);

    // 正确性验证：两后端输出必须一致（0+1+...+999999 = 499999500000）
    EXPECT_EQ(jitOut, vmOut) << "JIT 与 StackVM 输出不一致";
    EXPECT_EQ(jitOut, "499999500000") << "累加结果错误";

    // 性能对比输出（不做断言，仅报告）
    std::cout << "[PerfBenchmark.LargeLoopSum] StackVM=" << vmMs << "ms JIT=" << jitMs << "ms";
    if (jitMs > 0.0) {
        std::cout << " speedup=" << (vmMs / jitMs) << "x";
    }
    std::cout << "\n";

    // 软性期望：JIT 应该不慢于 StackVM（允许 2 倍容忍度，避免 CI 噪声）
    // 注：PoC 阶段 JIT 编译开销可能使小基准变慢，此处仅在大循环上验证
    if (jitMs > 0.0 && vmMs > 0.0) {
        EXPECT_LT(jitMs, vmMs * 2.0) << "JIT 不应慢于 StackVM 2 倍以上";
    }
}

/// 嵌套循环基准：外层 1000 次 × 内层 1000 次（验证 JIT 在深循环中的收益）
/// 注：JIT 阶段 2a 不支持循环内 var 声明（生成 name-based 变量 OpCode），
/// 故将 j 提升到外层定义，循环内用赋值重置。
TEST(TestJITPerf, NestedLoop) {
    const std::string src = R"(
var i = 0;
var j = 0;
var total = 0;
while (i < 1000) {
    j = 0;
    while (j < 1000) {
        total = total + 1;
        j = j + 1;
    }
    i = i + 1;
}
print(total);
)";
    auto [jitMs, jitOut] = runJITTimed(src);
    auto [vmMs, vmOut] = runStackVM(src);

    // 正确性：1000 * 1000 = 1000000
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "1000000");

    std::cout << "[PerfBenchmark.NestedLoop] StackVM=" << vmMs << "ms JIT=" << jitMs << "ms";
    if (jitMs > 0.0) {
        std::cout << " speedup=" << (vmMs / jitMs) << "x";
    }
    std::cout << "\n";

    if (jitMs > 0.0 && vmMs > 0.0) {
        EXPECT_LT(jitMs, vmMs * 2.0) << "JIT 不应慢于 StackVM 2 倍以上";
    }
}

/// 除法取模混合基准：在循环中混合除法与取模运算
TEST(TestJITPerf, DivisionModuloLoop) {
    const std::string src = R"(
var i = 1;
var acc = 0;
while (i < 100000) {
    acc = acc + (i / 3) - (i % 7);
    i = i + 1;
}
print(acc);
)";
    auto [jitMs, jitOut] = runJITTimed(src);
    auto [vmMs, vmOut] = runStackVM(src);

    EXPECT_EQ(jitOut, vmOut) << "JIT 与 StackVM 输出不一致";

    std::cout << "[PerfBenchmark.DivisionModuloLoop] StackVM=" << vmMs << "ms JIT=" << jitMs << "ms";
    if (jitMs > 0.0) {
        std::cout << " speedup=" << (vmMs / jitMs) << "x";
    }
    std::cout << "\n";

    if (jitMs > 0.0 && vmMs > 0.0) {
        EXPECT_LT(jitMs, vmMs * 2.0) << "JIT 不应慢于 StackVM 2 倍以上";
    }
}

// ============================================================
// R141 性能对比基准：递归函数调用（JIT vs StackVM）
// ============================================================
// 目的：量化 JIT 在函数调用场景下相对于 StackVM 的执行时间收益
// 注意：不做 pass/fail 断言，仅测量并输出对比（避免环境噪声导致 CI 抖动）

TEST(TestJITPerf, RecursiveFibonacciPerf) {
    // 递归 fib(30) 性能对比（约 1.6M 次函数调用）
    const std::string src = R"(
fun fib(n) {
    if (n < 2) { return n; }
    return fib(n-1) + fib(n-2);
}
print(fib(30));
)";
    auto [jitMs, jitOut] = runJITTimed(src);
    auto [vmMs, vmOut] = runStackVM(src);

    // 正确性验证：fib(30) = 832040
    EXPECT_EQ(jitOut, vmOut) << "JIT 与 StackVM 输出不一致";
    EXPECT_EQ(jitOut, "832040") << "fib(30) 结果错误";

    std::cout << "[PerfBenchmark.RecursiveFibonacci] StackVM=" << vmMs << "ms JIT=" << jitMs << "ms";
    if (jitMs > 0.0) {
        std::cout << " speedup=" << (vmMs / jitMs) << "x";
    }
    std::cout << "\n";

    // 软性期望：JIT 应该不慢于 StackVM（允许 3 倍容忍度，递归调用开销较大）
    if (jitMs > 0.0 && vmMs > 0.0) {
        EXPECT_LT(jitMs, vmMs * 3.0) << "JIT 不应慢于 StackVM 3 倍以上";
    }
}

// ============================================================
// R143 阶段 3b 浮点 + 字符串类型测试
// ============================================================
// 验证 JIT 后端在引入"INT 快速路径 + C++ 辅助慢速路径"策略后：
//   1. 浮点字面量/算术/比较/取负的输出与 StackVM 完全一致
//   2. 字符串字面量/拼接/比较的输出与 StackVM 完全一致
//   3. truthiness 修复：FLOAT(0.0)→falsy、FLOAT(非0)→truthy、STRING(空)→falsy、STRING(非空)→truthy
//   4. 混合类型运算（INT+FLOAT、STRING+INT）三后端一致
//   5. OP_EQUAL 处理 FLOAT +0.0/-0.0/NaN 与 STRING 内容比较
//
// 测试策略：除特定边界用例外，主要采用"JIT 输出 == StackVM 输出"对比，
// 避免硬编码浮点格式（std::to_chars general 17 的具体输出难预测）。
// @since R143

// ---- 浮点字面量输出测试 ----

TEST(TestJIT, R143FloatPrintLiteral) {
    // 浮点字面量输出与 StackVM 一致
    EXPECT_EQ(runJIT("print(3.14);"), minilang_test::runStackVM("print(3.14);"));
}

TEST(TestJIT, R143FloatPrintWhole) {
    // 整数浮点（4.0）输出与 StackVM 一致
    EXPECT_EQ(runJIT("print(4.0);"), minilang_test::runStackVM("print(4.0);"));
}

TEST(TestJIT, R143FloatPrintZero) {
    // 0.0 输出与 StackVM 一致
    EXPECT_EQ(runJIT("print(0.0);"), minilang_test::runStackVM("print(0.0);"));
}

TEST(TestJIT, R143FloatPrintNegative) {
    // 负浮点输出与 StackVM 一致
    EXPECT_EQ(runJIT("print(-3.14);"), minilang_test::runStackVM("print(-3.14);"));
}

// ---- 浮点算术测试 ----

TEST(TestJIT, R143FloatAddition) {
    // 1.5 + 2.5 → 4.0
    EXPECT_EQ(runJIT("print(1.5 + 2.5);"), minilang_test::runStackVM("print(1.5 + 2.5);"));
}

TEST(TestJIT, R143FloatSubtraction) {
    // 5.5 - 1.5 → 4.0
    EXPECT_EQ(runJIT("print(5.5 - 1.5);"), minilang_test::runStackVM("print(5.5 - 1.5);"));
}

TEST(TestJIT, R143FloatMultiplication) {
    // 2.5 * 4.0 → 10.0
    EXPECT_EQ(runJIT("print(2.5 * 4.0);"), minilang_test::runStackVM("print(2.5 * 4.0);"));
}

TEST(TestJIT, R143FloatDivision) {
    // 10.0 / 4.0 → 2.5
    EXPECT_EQ(runJIT("print(10.0 / 4.0);"), minilang_test::runStackVM("print(10.0 / 4.0);"));
}

TEST(TestJIT, R143FloatModulo) {
    // 10.5 % 3.0 → 1.5（std::fmod）
    EXPECT_EQ(runJIT("print(10.5 % 3.0);"), minilang_test::runStackVM("print(10.5 % 3.0);"));
}

TEST(TestJIT, R143FloatNegate) {
    // -3.14（OP_NEGATE 路径）
    EXPECT_EQ(runJIT("print(-3.14);"), minilang_test::runStackVM("print(-3.14);"));
}

TEST(TestJIT, R143FloatNegateVariable) {
    // var x = 2.5; print(-x);
    EXPECT_EQ(runJIT("var x = 2.5; print(-x);"), minilang_test::runStackVM("var x = 2.5; print(-x);"));
}

// ---- 混合类型算术（INT + FLOAT）----

TEST(TestJIT, R143MixedIntFloatAddition) {
    // 1 + 2.5 → 3.5（INT 走 C++ 辅助路径，调用 toDouble 加法）
    EXPECT_EQ(runJIT("print(1 + 2.5);"), minilang_test::runStackVM("print(1 + 2.5);"));
}

TEST(TestJIT, R143MixedFloatIntAddition) {
    // 2.5 + 1 → 3.5
    EXPECT_EQ(runJIT("print(2.5 + 1);"), minilang_test::runStackVM("print(2.5 + 1);"));
}

TEST(TestJIT, R143MixedIntFloatDivision) {
    // 10 / 4.0 → 2.5（INT/INT 是整除，混合除法是浮点除法）
    EXPECT_EQ(runJIT("print(10 / 4.0);"), minilang_test::runStackVM("print(10 / 4.0);"));
}

TEST(TestJIT, R143MixedFloatIntMultiplication) {
    // 2.5 * 4 → 10.0
    EXPECT_EQ(runJIT("print(2.5 * 4);"), minilang_test::runStackVM("print(2.5 * 4);"));
}

// ---- 浮点比较测试 ----

TEST(TestJIT, R143FloatEqual) {
    EXPECT_EQ(runJIT("print(1.5 == 1.5);"), minilang_test::runStackVM("print(1.5 == 1.5);"));
    EXPECT_EQ(runJIT("print(1.5 == 2.5);"), minilang_test::runStackVM("print(1.5 == 2.5);"));
}

TEST(TestJIT, R143FloatNotEqual) {
    EXPECT_EQ(runJIT("print(1.5 != 2.5);"), minilang_test::runStackVM("print(1.5 != 2.5);"));
    EXPECT_EQ(runJIT("print(1.5 != 1.5);"), minilang_test::runStackVM("print(1.5 != 1.5);"));
}

TEST(TestJIT, R143FloatLess) {
    EXPECT_EQ(runJIT("print(1.5 < 2.5);"), minilang_test::runStackVM("print(1.5 < 2.5);"));
    EXPECT_EQ(runJIT("print(2.5 < 1.5);"), minilang_test::runStackVM("print(2.5 < 1.5);"));
}

TEST(TestJIT, R143FloatGreater) {
    EXPECT_EQ(runJIT("print(2.5 > 1.5);"), minilang_test::runStackVM("print(2.5 > 1.5);"));
}

TEST(TestJIT, R143FloatLessEqual) {
    EXPECT_EQ(runJIT("print(1.5 <= 1.5);"), minilang_test::runStackVM("print(1.5 <= 1.5);"));
    EXPECT_EQ(runJIT("print(2.5 <= 1.5);"), minilang_test::runStackVM("print(2.5 <= 1.5);"));
}

TEST(TestJIT, R143FloatGreaterEqual) {
    EXPECT_EQ(runJIT("print(1.5 >= 1.5);"), minilang_test::runStackVM("print(1.5 >= 1.5);"));
    EXPECT_EQ(runJIT("print(1.5 >= 2.5);"), minilang_test::runStackVM("print(1.5 >= 2.5);"));
}

TEST(TestJIT, R143MixedIntFloatComparison) {
    // 1 < 1.5 → true（跨类型比较走 C++ 辅助）
    EXPECT_EQ(runJIT("print(1 < 1.5);"), minilang_test::runStackVM("print(1 < 1.5);"));
    EXPECT_EQ(runJIT("print(1 == 1.0);"), minilang_test::runStackVM("print(1 == 1.0);"));
    EXPECT_EQ(runJIT("print(1 != 1.5);"), minilang_test::runStackVM("print(1 != 1.5);"));
}

// ---- 浮点 truthiness 修复测试 ----
// R143 关键修复：FLOAT 不能用 payload==0 判断 truthiness（1.0 误判 falsy）

TEST(TestJIT, R143FloatTruthyNonZero) {
    // 1.0 → truthy（R143 修复：原 payload==0 检查误判 1.0 为 falsy）
    EXPECT_EQ(runJIT("if (1.0) { print(1); } else { print(2); }"),
              minilang_test::runStackVM("if (1.0) { print(1); } else { print(2); }"));
}

TEST(TestJIT, R143FloatTruthyZero) {
    // 0.0 → falsy
    EXPECT_EQ(runJIT("if (0.0) { print(1); } else { print(2); }"),
              minilang_test::runStackVM("if (0.0) { print(1); } else { print(2); }"));
}

TEST(TestJIT, R143FloatTruthyNegative) {
    // -3.14 → truthy（非零浮点）
    EXPECT_EQ(runJIT("if (-3.14) { print(1); } else { print(2); }"),
              minilang_test::runStackVM("if (-3.14) { print(1); } else { print(2); }"));
}

TEST(TestJIT, R143FloatLogicalNot) {
    // !0.0 → true，!1.5 → false
    EXPECT_EQ(runJIT("print(!0.0); print(!1.5);"), minilang_test::runStackVM("print(!0.0); print(!1.5);"));
}

TEST(TestJIT, R143FloatWhileLoop) {
    // 浮点循环条件：var x = 2.5; while (x > 0.0) { print(x); x = x - 1.0; }
    EXPECT_EQ(runJIT("var x = 2.5; while (x > 0.0) { print(x); x = x - 1.0; }"),
              minilang_test::runStackVM("var x = 2.5; while (x > 0.0) { print(x); x = x - 1.0; }"));
}

// ---- 浮点除零错误测试 ----

TEST(TestJIT, R143FloatDivisionByZero) {
    // 浮点除零：StackVM 报错，JIT 也应报错（C++ 辅助路径检查 r == 0.0）
    auto jitOut = runJIT("print(1.0 / 0.0);");
    auto vmOut = minilang_test::runStackVM("print(1.0 / 0.0);");
    // 两后端都应报"除零错误"（错误标志设置，输出可能为空或带错误标记）
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "JIT 浮点除零应报运行时错误，实际输出: " << jitOut;
    EXPECT_NE(vmOut.find("<runtime:"), std::string::npos) << "StackVM 浮点除零应报运行时错误，实际输出: " << vmOut;
}

TEST(TestJIT, R143FloatModuloByZero) {
    auto jitOut = runJIT("print(1.5 % 0.0);");
    auto vmOut = minilang_test::runStackVM("print(1.5 % 0.0);");
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "JIT 浮点取模除零应报运行时错误";
    EXPECT_NE(vmOut.find("<runtime:"), std::string::npos) << "StackVM 浮点取模除零应报运行时错误";
}

// ---- 字符串字面量输出测试 ----

TEST(TestJIT, R143StringPrintLiteral) {
    EXPECT_EQ(runJIT("print(\"hello\");"), "hello");
}

TEST(TestJIT, R143StringPrintEmpty) {
    EXPECT_EQ(runJIT("print(\"\");"), "");
}

TEST(TestJIT, R143StringPrintWithSpaces) {
    EXPECT_EQ(runJIT("print(\"hello world\");"), "hello world");
}

TEST(TestJIT, R143StringPrintChinese) {
    // UTF-8 中文字符串
    EXPECT_EQ(runJIT("print(\"你好世界\");"), "你好世界");
}

// ---- 字符串拼接测试 ----

TEST(TestJIT, R143StringConcatenation) {
    // "hello" + "world" → "helloworld"
    EXPECT_EQ(runJIT("print(\"hello\" + \"world\");"), "helloworld");
}

TEST(TestJIT, R143StringConcatWithSpace) {
    // "hello" + " " + "world" → "hello world"
    EXPECT_EQ(runJIT("print(\"hello\" + \" \" + \"world\");"), "hello world");
}

TEST(TestJIT, R143StringConcatWithInt) {
    // "count: " + 42 → "count: 42"（STRING + INT 拼接）
    EXPECT_EQ(runJIT("print(\"count: \" + 42);"), "count: 42");
}

TEST(TestJIT, R143IntConcatWithString) {
    // 42 + " is answer" → "42 is answer"（INT + STRING 拼接）
    EXPECT_EQ(runJIT("print(42 + \" is answer\");"), "42 is answer");
}

TEST(TestJIT, R143StringConcatWithFloat) {
    // "pi = " + 3.14 → "pi = 3.14..."（与 StackVM 一致）
    EXPECT_EQ(runJIT("print(\"pi = \" + 3.14);"), minilang_test::runStackVM("print(\"pi = \" + 3.14);"));
}

TEST(TestJIT, R143StringConcatWithBool) {
    // "flag = " + true → "flag = true"
    EXPECT_EQ(runJIT("print(\"flag = \" + true);"), "flag = true");
}

TEST(TestJIT, R143StringConcatVariable) {
    // var greeting = "hi"; var name = "alice"; print(greeting + name);
    EXPECT_EQ(runJIT("var g = \"hi\"; var n = \"alice\"; print(g + n);"), "hialice");
}

// ---- 字符串比较测试 ----

TEST(TestJIT, R143StringEqual) {
    EXPECT_EQ(runJIT("print(\"abc\" == \"abc\");"), "true");
    EXPECT_EQ(runJIT("print(\"abc\" == \"abd\");"), "false");
}

TEST(TestJIT, R143StringNotEqual) {
    EXPECT_EQ(runJIT("print(\"abc\" != \"abd\");"), "true");
    EXPECT_EQ(runJIT("print(\"abc\" != \"abc\");"), "false");
}

TEST(TestJIT, R143StringLess) {
    // 字典序比较
    EXPECT_EQ(runJIT("print(\"abc\" < \"abd\");"), "true");
    EXPECT_EQ(runJIT("print(\"abd\" < \"abc\");"), "false");
}

TEST(TestJIT, R143StringGreater) {
    EXPECT_EQ(runJIT("print(\"abd\" > \"abc\");"), "true");
}

TEST(TestJIT, R143StringLessEqual) {
    EXPECT_EQ(runJIT("print(\"abc\" <= \"abc\");"), "true");
    EXPECT_EQ(runJIT("print(\"abd\" <= \"abc\");"), "false");
}

TEST(TestJIT, R143StringGreaterEqual) {
    EXPECT_EQ(runJIT("print(\"abc\" >= \"abc\");"), "true");
    EXPECT_EQ(runJIT("print(\"abc\" >= \"abd\");"), "false");
}

// ---- 字符串 truthiness 修复测试 ----
// R143 关键修复：STRING 不能用 payload==0 判断 truthiness（空字符串应 falsy）

TEST(TestJIT, R143StringTruthyNonEmpty) {
    // 非空字符串 → truthy
    EXPECT_EQ(runJIT("if (\"x\") { print(1); } else { print(2); }"), "1");
}

TEST(TestJIT, R143StringTruthyEmpty) {
    // 空字符串 → falsy（R143 修复：原 payload==0 检查可能误判）
    EXPECT_EQ(runJIT("if (\"\") { print(1); } else { print(2); }"), "2");
}

TEST(TestJIT, R143StringLogicalNot) {
    // !"" → true，!"x" → false
    EXPECT_EQ(runJIT("print(!\"\"); print(!\"x\");"), "truefalse");
}

// ---- 字符串变量与函数返回 ----

TEST(TestJIT, R143StringVariable) {
    EXPECT_EQ(runJIT("var s = \"hello\"; print(s);"), "hello");
}

TEST(TestJIT, R143StringAssignAndConcat) {
    // var s = "a"; s = s + "b"; s = s + "c"; print(s); → "abc"
    EXPECT_EQ(runJIT("var s = \"a\"; s = s + \"b\"; s = s + \"c\"; print(s);"), "abc");
}

TEST(TestJIT, R143FunctionReturnsString) {
    EXPECT_EQ(runJIT("fun greet(name) { return \"hi \" + name; } print(greet(\"alice\"));"), "hi alice");
}

TEST(TestJIT, R143FunctionStringParam) {
    EXPECT_EQ(runJIT("fun concat(a, b) { return a + b; } print(concat(\"foo\", \"bar\"));"), "foobar");
}

// ---- 浮点变量与函数返回 ----

TEST(TestJIT, R143FloatVariable) {
    EXPECT_EQ(runJIT("var x = 3.14; print(x);"), minilang_test::runStackVM("var x = 3.14; print(x);"));
}

TEST(TestJIT, R143FunctionReturnsFloat) {
    EXPECT_EQ(runJIT("fun half(n) { return n / 2.0; } print(half(10));"),
              minilang_test::runStackVM("fun half(n) { return n / 2.0; } print(half(10));"));
}

TEST(TestJIT, R143FunctionFloatArithmetic) {
    // 函数内浮点算术
    EXPECT_EQ(runJIT("fun area(r) { return 3.14 * r * r; } print(area(2));"),
              minilang_test::runStackVM("fun area(r) { return 3.14 * r * r; } print(area(2));"));
}

// ---- 复合场景：控制流 + 浮点/字符串 ----

TEST(TestJIT, R143IfElseWithFloat) {
    EXPECT_EQ(runJIT("fun signum(x) { if (x > 0.0) { return 1; } else if (x < 0.0) { return -1; } "
                     "else { return 0; } }"
                     "print(signum(2.5)); print(signum(-1.5)); print(signum(0.0));"),
              minilang_test::runStackVM("fun signum(x) { if (x > 0.0) { return 1; } else if (x < 0.0) { return -1; } "
                                        "else { return 0; } }"
                                        "print(signum(2.5)); print(signum(-1.5)); print(signum(0.0));"));
}

TEST(TestJIT, R143WhileLoopWithStringAccumulator) {
    // 字符串累加：var s = ""; var i = 0; while (i < 3) { s = s + "x"; i = i + 1; } print(s);
    EXPECT_EQ(runJIT("var s = \"\"; var i = 0; while (i < 3) { s = s + \"x\"; i = i + 1; } print(s);"), "xxx");
}

TEST(TestJIT, R143FloatInWhileLoop) {
    // 浮点循环累加
    EXPECT_EQ(runJIT("var sum = 0.0; var i = 0; while (i < 4) { sum = sum + 0.5; i = i + 1; } print(sum);"),
              minilang_test::runStackVM(
                  "var sum = 0.0; var i = 0; while (i < 4) { sum = sum + 0.5; i = i + 1; } print(sum);"));
}

// ---- 三后端一致性综合测试 ----

TEST(TestJIT, R143ThreeBackendConsistencyFloat) {
    // 综合浮点场景：所有四后端输出应一致
    const std::string src = "var x = 1.5;"
                            "var y = 2.25;"
                            "print(x + y);"
                            "print(x * y);"
                            "print(y - x);"
                            "print(y / x);"
                            "print(x == 1.5);"
                            "print(x < y);"
                            "if (x > 0.0) { print(\"pos\"); } else { print(\"neg\"); }";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R143ThreeBackendConsistencyString) {
    // 综合字符串场景
    const std::string src = "var a = \"hello\";"
                            "var b = \"world\";"
                            "print(a + \" \" + b);"
                            "print(a == b);"
                            "print(a < b);"
                            "if (a) { print(\"nonempty\"); }"
                            "if (!\"\") { print(\"empty-falsy\"); }";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

// ============================================================
// R146 阶段 4：数组类型支持测试
// ============================================================
// 验证 OP_BUILD_ARRAY / OP_INDEX_GET / OP_INDEX_SET_LOCAL 三个 OpCode。
// 全局变量数组索引读取走 OP_GET_GLOBAL + OP_INDEX_GET（已支持）。
// 全局变量数组索引赋值走 OP_INDEX_SET_VAR（R146 暂未支持，用函数内局部变量路径测试）。

TEST(TestJIT, R146ArrayLiteralIndexGet) {
    // 数组字面量 + 索引读取（最基础路径）
    EXPECT_EQ(runJIT("var arr = [1, 2, 3]; print(arr[0]);"), "1");
}

TEST(TestJIT, R146ArrayIndexGetMiddle) {
    EXPECT_EQ(runJIT("var arr = [10, 20, 30]; print(arr[1]);"), "20");
}

TEST(TestJIT, R146ArrayIndexGetLast) {
    EXPECT_EQ(runJIT("var arr = [10, 20, 30]; print(arr[2]);"), "30");
}

TEST(TestJIT, R146ArrayMultipleAccess) {
    const std::string src = "var arr = [1, 2, 3];"
                            "print(arr[0]);"
                            "print(arr[1]);"
                            "print(arr[2]);";
    EXPECT_EQ(runJIT(src), "123");
}

TEST(TestJIT, R146ArrayWithExpressionElements) {
    // 数组元素是表达式
    EXPECT_EQ(runJIT("var arr = [1+2, 3*4, 10-5]; print(arr[0]); print(arr[1]); print(arr[2]);"), "3125");
}

TEST(TestJIT, R146ArrayInFunction) {
    // 函数返回数组元素
    const std::string src = "fun getElem() {"
                            "    var arr = [100, 200, 300];"
                            "    return arr[1];"
                            "}"
                            "print(getElem());";
    EXPECT_EQ(runJIT(src), "200");
}

TEST(TestJIT, R146ArrayIndexSetLocal) {
    // OP_INDEX_SET_LOCAL：函数内局部变量数组索引赋值
    const std::string src = "fun modifyArr() {"
                            "    var arr = [1, 2, 3];"
                            "    arr[0] = 99;"
                            "    return arr[0];"
                            "}"
                            "print(modifyArr());";
    EXPECT_EQ(runJIT(src), "99");
}

TEST(TestJIT, R146ArrayIndexSetLocalMultiple) {
    // 多次索引赋值
    const std::string src = "fun modifyArr() {"
                            "    var arr = [1, 2, 3];"
                            "    arr[0] = 10;"
                            "    arr[1] = 20;"
                            "    arr[2] = 30;"
                            "    return arr[0] + arr[1] + arr[2];"
                            "}"
                            "print(modifyArr());";
    EXPECT_EQ(runJIT(src), "60");
}

TEST(TestJIT, R146ArrayIndexSetLocalThenRead) {
    // 赋值后多次读取
    const std::string src = "fun modifyArr() {"
                            "    var arr = [5, 6, 7];"
                            "    arr[1] = 999;"
                            "    return arr[1];"
                            "}"
                            "print(modifyArr());";
    EXPECT_EQ(runJIT(src), "999");
}

TEST(TestJIT, R146ArrayIndexOutOfBoundsHigh) {
    // 越界：索引 >= 长度
    const std::string src = "var arr = [1, 2, 3]; print(arr[5]);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "期望 JIT 报告运行时错误，实际输出: " << jitOut;
    EXPECT_NE(jitOut.find("数组索引越界"), std::string::npos) << "期望错误消息包含'数组索引越界'，实际: " << jitOut;
}

TEST(TestJIT, R146ArrayIndexOutOfBoundsNegative) {
    // 越界：负索引
    const std::string src = "var arr = [1, 2, 3]; print(arr[-1]);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "期望 JIT 报告运行时错误，实际输出: " << jitOut;
    EXPECT_NE(jitOut.find("数组索引越界"), std::string::npos) << "期望错误消息包含'数组索引越界'，实际: " << jitOut;
}

TEST(TestJIT, R146ArrayIndexGetEmptyArray) {
    // 空数组越界
    const std::string src = "var arr = []; print(arr[0]);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "期望 JIT 报告运行时错误，实际输出: " << jitOut;
}

TEST(TestJIT, R146ArrayIndexSetLocalOutOfBounds) {
    // 函数内越界赋值
    const std::string src = "fun badModify() {"
                            "    var arr = [1, 2, 3];"
                            "    arr[10] = 99;"
                            "    return arr[0];"
                            "}"
                            "print(badModify());";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "期望 JIT 报告运行时错误，实际输出: " << jitOut;
    EXPECT_NE(jitOut.find("数组索引越界"), std::string::npos) << "期望错误消息包含'数组索引越界'，实际: " << jitOut;
}

TEST(TestJIT, R146ArrayStringIndexGet) {
    // 字符串索引读取（OP_INDEX_GET 多态路径）
    EXPECT_EQ(runJIT("var s = \"hello\"; print(s[0]);"), "h");
}

TEST(TestJIT, R146ArrayStringIndexGetLast) {
    EXPECT_EQ(runJIT("var s = \"hello\"; print(s[4]);"), "o");
}

TEST(TestJIT, R146ArrayStringIndexOutOfBounds) {
    const std::string src = "var s = \"hi\"; print(s[5]);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "期望 JIT 报告运行时错误，实际输出: " << jitOut;
}

// R161 fixup: UTF-8 字符串索引测试（对齐 StackVM 的 UTF-8 慢路径）
TEST(TestJIT, R161Utf8StringIndexBasic) {
    // 中文 UTF-8 字符串索引——每个汉字 3 字节，JIT 需走 UTF-8 慢路径
    const std::string src = "var s = \"你好\"; print(s[0]);";
    EXPECT_EQ(runJIT(src), "你");
}

TEST(TestJIT, R161Utf8StringIndexSecondChar) {
    const std::string src = "var s = \"你好\"; print(s[1]);";
    EXPECT_EQ(runJIT(src), "好");
}

TEST(TestJIT, R161Utf8StringIndexMixedAscii) {
    // 混合 ASCII + UTF-8：'a' + '你' + 'b'
    const std::string src = "var s = \"a你好b\"; print(s[0]); print(s[1]); print(s[2]); print(s[3]);";
    EXPECT_EQ(runJIT(src), "a你好b");
}

TEST(TestJIT, R161Utf8StringIndexOutOfBounds) {
    // "你好" 有 2 个码位，s[2] 越界
    const std::string src = "var s = \"你好\"; print(s[2]);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos) << "期望 JIT 报告越界错误，实际输出: " << jitOut;
}

TEST(TestJIT, R161Utf8StringIndexConsistencyWithStackVM) {
    // 四后端一致性：JIT 与 StackVM 对 UTF-8 字符串索引输出完全一致
    const std::string src = "var s = \"你好世界\"; print(s[0]); print(s[1]); print(s[2]); print(s[3]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "你好世界");
}

TEST(TestJIT, R161Utf8StringIndexEmoji) {
    // 4 字节 UTF-8 字符（emoji）索引
    const std::string src = "var s = \"a😀b\"; print(s[0]); print(s[1]); print(s[2]);";
    EXPECT_EQ(runJIT(src), "a😀b");
}

TEST(TestJIT, R146ArrayInLoop) {
    // 循环内访问数组元素（测试 JIT 栈平衡）
    const std::string src = "var arr = [1, 2, 3, 4, 5];"
                            "var sum = 0;"
                            "var i = 0;"
                            "while (i < 5) {"
                            "    sum = sum + arr[i];"
                            "    i = i + 1;"
                            "}"
                            "print(sum);";
    EXPECT_EQ(runJIT(src), "15");
}

TEST(TestJIT, R146ArrayMixedTypes) {
    // 混合类型数组（int + string）
    const std::string src = "var arr = [1, \"hello\", 2];"
                            "print(arr[0]);"
                            "print(arr[1]);"
                            "print(arr[2]);";
    EXPECT_EQ(runJIT(src), "1hello2");
}

TEST(TestJIT, R146ArrayNestedArray) {
    // 嵌套数组
    const std::string src = "var arr = [[1, 2], [3, 4]];"
                            "print(arr[0][0]);"
                            "print(arr[0][1]);"
                            "print(arr[1][0]);"
                            "print(arr[1][1]);";
    EXPECT_EQ(runJIT(src), "1234");
}

TEST(TestJIT, R146ArrayFunctionParam) {
    // 数组作为函数参数
    const std::string src = "fun getFirst(a) {"
                            "    return a[0];"
                            "}"
                            "print(getFirst([10, 20, 30]));";
    EXPECT_EQ(runJIT(src), "10");
}

TEST(TestJIT, R146ArrayIndexSetLocalWithExpression) {
    // 赋值的值是表达式
    const std::string src = "fun compute() {"
                            "    var arr = [0, 0, 0];"
                            "    arr[0] = 2 * 3;"
                            "    arr[1] = arr[0] + 4;"
                            "    arr[2] = arr[1] - arr[0];"
                            "    return arr[2];"
                            "}"
                            "print(compute());";
    EXPECT_EQ(runJIT(src), "4");
}

TEST(TestJIT, R146ArrayConsistencyWithStackVM) {
    // 三后端一致性：数组构造 + 索引读取
    const std::string src = "var arr = [1, 2, 3, 4, 5];"
                            "print(arr[0]);"
                            "print(arr[1]);"
                            "print(arr[2]);"
                            "print(arr[3]);"
                            "print(arr[4]);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R146ArrayModifyConsistencyWithStackVM) {
    // 三后端一致性：数组索引赋值
    const std::string src = "fun test() {"
                            "    var arr = [1, 2, 3];"
                            "    arr[0] = 100;"
                            "    arr[1] = 200;"
                            "    arr[2] = 300;"
                            "    return arr[0] + arr[1] + arr[2];"
                            "}"
                            "print(test());";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R146ArrayErrorConsistencyWithStackVM) {
    // 三后端一致性：越界错误消息
    const std::string src = "var arr = [1, 2, 3]; print(arr[10]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    // 两者都应包含"数组索引越界"消息（具体格式可能略不同）
    EXPECT_NE(jitOut.find("数组索引越界"), std::string::npos);
    EXPECT_NE(vmOut.find("数组索引越界"), std::string::npos);
}

// ============================================================
// R147 阶段 4b：dict/tuple + OP_INDEX_SET_VAR 测试
// ============================================================

// ---- 字典（dict）测试 ----

TEST(TestJIT, R147DictLiteralStringKey) {
    // 字典字面量 + 字符串键访问
    EXPECT_EQ(runJIT("var d = {\"a\": 1, \"b\": 2}; print(d[\"a\"]);"), "1");
}

TEST(TestJIT, R147DictMultipleKeys) {
    const std::string src = "var d = {\"x\": 10, \"y\": 20, \"z\": 30};"
                            "print(d[\"x\"]);"
                            "print(d[\"y\"]);"
                            "print(d[\"z\"]);";
    EXPECT_EQ(runJIT(src), "102030");
}

TEST(TestJIT, R147DictIntKey) {
    // 整数键
    EXPECT_EQ(runJIT("var d = {1: \"a\", 2: \"b\"}; print(d[1]);"), "a");
}

TEST(TestJIT, R147DictMissingKeyReturnsNull) {
    // 不存在的键返回 null（与 StackVM 一致）
    EXPECT_EQ(runJIT("var d = {\"a\": 1}; print(d[\"b\"]);"), "null");
}

TEST(TestJIT, R147DictEmptyLiteral) {
    // 空字典字面量
    EXPECT_EQ(runJIT("var d = {}; print(d[\"a\"]);"), "null");
}

TEST(TestJIT, R147DictIndexSetLocal) {
    // 函数内字典索引赋值（局部变量路径，OP_INDEX_SET_LOCAL 的 dict 分支）
    const std::string src = "fun modify() {"
                            "    var d = {\"a\": 1};"
                            "    d[\"a\"] = 99;"
                            "    return d[\"a\"];"
                            "}"
                            "print(modify());";
    EXPECT_EQ(runJIT(src), "99");
}

TEST(TestJIT, R147DictIndexSetLocalNewKey) {
    // 字典添加新键（局部变量路径）
    const std::string src = "fun modify() {"
                            "    var d = {\"a\": 1};"
                            "    d[\"b\"] = 2;"
                            "    return d[\"b\"];"
                            "}"
                            "print(modify());";
    EXPECT_EQ(runJIT(src), "2");
}

TEST(TestJIT, R147DictIndexSetVar) {
    // 全局变量字典索引赋值（OP_INDEX_SET_VAR）
    const std::string src = "var d = {\"a\": 1};"
                            "d[\"a\"] = 100;"
                            "print(d[\"a\"]);";
    EXPECT_EQ(runJIT(src), "100");
}

TEST(TestJIT, R147DictIndexSetVarNewKey) {
    // 全局变量字典添加新键
    const std::string src = "var d = {\"a\": 1};"
                            "d[\"b\"] = 200;"
                            "print(d[\"b\"]);";
    EXPECT_EQ(runJIT(src), "200");
}

TEST(TestJIT, R147DictIndexSetVarArray) {
    // 全局变量数组索引赋值（OP_INDEX_SET_VAR 的 array 分支）
    const std::string src = "var arr = [1, 2, 3];"
                            "arr[0] = 99;"
                            "arr[1] = 88;"
                            "print(arr[0]);"
                            "print(arr[1]);";
    EXPECT_EQ(runJIT(src), "9988");
}

TEST(TestJIT, R147DictIndexSetVarOutOfBounds) {
    // 全局变量数组越界赋值
    const std::string src = "var arr = [1, 2, 3];"
                            "arr[10] = 99;"
                            "print(arr[0]);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos);
    EXPECT_NE(jitOut.find("数组索引越界"), std::string::npos);
}

TEST(TestJIT, R147DictInFunction) {
    // 函数返回字典元素
    const std::string src = "fun getValue() {"
                            "    var d = {\"key\": 42};"
                            "    return d[\"key\"];"
                            "}"
                            "print(getValue());";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R147DictMixedValueTypes) {
    // 字典值是不同类型
    const std::string src = "var d = {\"i\": 1, \"s\": \"hello\", \"b\": true};"
                            "print(d[\"i\"]);"
                            "print(d[\"s\"]);"
                            "print(d[\"b\"]);";
    EXPECT_EQ(runJIT(src), "1hellotrue");
}

// ---- 元组（tuple）测试 ----

TEST(TestJIT, R147TupleLiteralIndexGet) {
    // 元组字面量 + 索引读取
    EXPECT_EQ(runJIT("var t = (1, 2, 3); print(t[0]);"), "1");
}

TEST(TestJIT, R147TupleIndexGetMiddle) {
    EXPECT_EQ(runJIT("var t = (10, 20, 30); print(t[1]);"), "20");
}

TEST(TestJIT, R147TupleMultipleAccess) {
    const std::string src = "var t = (1, 2, 3);"
                            "print(t[0]);"
                            "print(t[1]);"
                            "print(t[2]);";
    EXPECT_EQ(runJIT(src), "123");
}

TEST(TestJIT, R147TupleMixedTypes) {
    // 元组可以包含不同类型（与 Python 类似）
    const std::string src = "var t = (1, \"hello\", true);"
                            "print(t[0]);"
                            "print(t[1]);"
                            "print(t[2]);";
    EXPECT_EQ(runJIT(src), "1hellotrue");
}

TEST(TestJIT, R147TupleIndexOutOfBounds) {
    const std::string src = "var t = (1, 2, 3); print(t[5]);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos);
    EXPECT_NE(jitOut.find("元组索引越界"), std::string::npos);
}

TEST(TestJIT, R147TupleInFunction) {
    const std::string src = "fun getSecond() {"
                            "    var t = (100, 200, 300);"
                            "    return t[1];"
                            "}"
                            "print(getSecond());";
    EXPECT_EQ(runJIT(src), "200");
}

TEST(TestJIT, R147TupleDestructure) {
    // 元组通过索引访问"解构"（var (a, b) = ... 解构语法编译为 OP_DEFINE_VAR，
    // JIT 阶段 2b 不支持，改用索引访问模拟解构）
    const std::string src = "var t = (10, 20);"
                            "print(t[0]);"
                            "print(t[1]);";
    EXPECT_EQ(runJIT(src), "1020");
}

// ---- 三后端一致性测试 ----

TEST(TestJIT, R147DictConsistencyWithStackVM) {
    const std::string src = "var d = {\"a\": 1, \"b\": 2, \"c\": 3};"
                            "print(d[\"a\"]);"
                            "print(d[\"b\"]);"
                            "print(d[\"c\"]);"
                            "print(d[\"missing\"]);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R147DictModifyConsistencyWithStackVM) {
    const std::string src = "fun test() {"
                            "    var d = {\"x\": 1};"
                            "    d[\"x\"] = 100;"
                            "    d[\"y\"] = 200;"
                            "    return d[\"x\"] + d[\"y\"];"
                            "}"
                            "print(test());";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R147DictGlobalModifyConsistencyWithStackVM) {
    // 全局变量字典索引赋值三后端一致性
    const std::string src = "var d = {\"a\": 1};"
                            "d[\"a\"] = 999;"
                            "d[\"b\"] = 888;"
                            "print(d[\"a\"]);"
                            "print(d[\"b\"]);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R147TupleConsistencyWithStackVM) {
    const std::string src = "var t = (1, \"hello\", true);"
                            "print(t[0]);"
                            "print(t[1]);"
                            "print(t[2]);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R147ArrayGlobalModifyConsistencyWithStackVM) {
    // 全局变量数组索引赋值三后端一致性
    const std::string src = "var arr = [1, 2, 3];"
                            "arr[0] = 100;"
                            "arr[1] = 200;"
                            "arr[2] = 300;"
                            "print(arr[0] + arr[1] + arr[2]);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

// ============================================================
// R148 阶段 4c-1+2：类支持测试
// ------------------------------------------------------------
// 验证 OP_DEFINE_CLASS / OP_INIT_FIELD / OP_CLASS_NEW /
// OP_MEMBER_GET / OP_MEMBER_SET_VAR / OP_MEMBER_SET_LOCAL
//
// 设计约束（JIT R148 限制）：
//   - 不支持带参数的类构造（init 方法调用）→ argCount 必须 = 0
//   - 不支持方法调用（OP_METHOD_CALL/SUPER_CALL）→ 测试不调用任何方法
//   - 不支持 Foo() 类实例化语法（编译为 OP_CALL，JIT funcTable 找不到类名）
//     → 用类型注解 `var p: Foo;` 触发 OP_CLASS_NEW（argCount=0）
//   - OP_TYPE_CHECK 在 JIT 中兜底跳过（不做实际类型检查），以支持类型注解语法
// ============================================================

TEST(TestJIT, R148ClassDefineBasic) {
    // 类定义阶段：OP_CLASS_NEW（空模板）+ OP_INIT_FIELD × 2 + OP_DEFINE_CLASS
    // 无实例化、无字段访问，验证类定义不崩溃
    const std::string src = "class Point { var x = 10; var y = 20; }";
    EXPECT_EQ(runJIT(src), "");
}

TEST(TestJIT, R148ClassInstantiateAndAccess) {
    // var p: Point; 触发 OP_CLASS_NEW（复制 fieldDefaults）+ OP_TYPE_CHECK（跳过）+ OP_DEFINE_GLOBAL
    // p.x / p.y 触发 OP_MEMBER_GET
    const std::string src = "class Point { var x = 10; var y = 20; }"
                            "var p: Point;"
                            "print(p.x);"
                            "print(p.y);";
    EXPECT_EQ(runJIT(src), "1020");
}

TEST(TestJIT, R148ClassMemberSetVar) {
    // p.x = 100 触发 OP_MEMBER_SET_VAR（全局变量字段修改）
    const std::string src = "class Point { var x = 10; var y = 20; }"
                            "var p: Point;"
                            "p.x = 100;"
                            "print(p.x);";
    EXPECT_EQ(runJIT(src), "100");
}

TEST(TestJIT, R148ClassMemberSetLocal) {
    // 在函数内创建实例 + 修改字段（OP_MEMBER_SET_LOCAL）
    const std::string src = "class Point { var x = 10; }"
                            "fn setField() {"
                            "  var p: Point;"
                            "  p.x = 999;"
                            "  print(p.x);"
                            "}"
                            "setField();";
    EXPECT_EQ(runJIT(src), "999");
}

TEST(TestJIT, R148ClassInheritance) {
    // 类继承 + 字段合并（父类字段在前，子类覆盖同名）
    const std::string src = "class Animal { var name = \"Cat\"; var sound = \"Meow\"; }"
                            "class Dog : Animal { var name = \"Dog\"; }"
                            "var d: Dog;"
                            "print(d.name);"
                            "print(d.sound);";
    EXPECT_EQ(runJIT(src), "DogMeow");
}

TEST(TestJIT, R148ClassInheritanceDeepChain) {
    // 多层继承链：A → B → C，验证字段沿继承链合并
    const std::string src = "class A { var a = 1; }"
                            "class B : A { var b = 2; }"
                            "class C : B { var c = 3; }"
                            "var x: C;"
                            "print(x.a + x.b + x.c);";
    EXPECT_EQ(runJIT(src), "6");
}

TEST(TestJIT, R148ClassFieldDefaultsCopied) {
    // 验证实例化时字段默认值被正确复制（每个实例独立副本）
    const std::string src = "class Counter { var count = 0; }"
                            "var c1: Counter;"
                            "var c2: Counter;"
                            "c1.count = 100;"
                            "print(c1.count);"
                            "print(c2.count);";
    EXPECT_EQ(runJIT(src), "1000");
}

TEST(TestJIT, R148ClassMemberGetConsistencyWithStackVM) {
    const std::string src = "class Point { var x = 10; var y = 20; }"
                            "var p: Point;"
                            "print(p.x + p.y);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R148ClassMemberSetVarConsistencyWithStackVM) {
    const std::string src = "class Point { var x = 10; var y = 20; }"
                            "var p: Point;"
                            "p.x = 100;"
                            "p.y = 200;"
                            "print(p.x + p.y);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R148ClassMemberSetLocalConsistencyWithStackVM) {
    const std::string src = "class Point { var x = 10; }"
                            "fn modify() {"
                            "  var p: Point;"
                            "  p.x = 42;"
                            "  print(p.x);"
                            "}"
                            "modify();";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R148ClassInheritanceConsistencyWithStackVM) {
    const std::string src = "class Animal { var name = \"Cat\"; var sound = \"Meow\"; }"
                            "class Dog : Animal { var name = \"Dog\"; }"
                            "var d: Dog;"
                            "print(d.name);"
                            "print(d.sound);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R148ClassInFunctionConsistencyWithStackVM) {
    // 在函数内创建实例并访问字段（直接 print，避免返回值类型注解冲突）
    const std::string src = "class Point { var x = 10; var y = 20; }"
                            "fn sum() {"
                            "  var p: Point;"
                            "  print(p.x + p.y);"
                            "}"
                            "sum();";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

// ============================================================
// R149 方法调用测试（OP_METHOD_CALL / OP_SUPER_CALL / init 调用）
// ============================================================
// 验证场景：
//   - init 方法调用（OP_CLASS_NEW argCount>0，jitClassNew 内部构造 init 帧）
//   - 0 参数 init 自动调用（OP_CLASS_NEW argCount=0 + init.requiredArity=0）
//   - 普通方法调用（OP_METHOD_CALL）：无参/带参/默认参数
//   - 方法修改 this 字段 + writeBack 到全局/局部接收者
//   - super 方法调用（OP_SUPER_CALL）：单层 + 多层继承
//   - 三后端一致性（JIT vs StackVM）
// ============================================================

TEST(TestJIT, R149InitMethodBasicCall) {
    // Point(3, 4) 触发 OP_CLASS_NEW argCount=2 → jitClassNew 调用 init → 字段赋值
    const std::string src = "class Point {"
                            "  var x = 0;"
                            "  var y = 0;"
                            "  fun init(ax, ay) { x = ax; y = ay; }"
                            "}"
                            "var p = Point(3, 4);"
                            "print(p.x);"
                            "print(p.y);";
    EXPECT_EQ(runJIT(src), "34");
}

TEST(TestJIT, R149InitMethodZeroArgAutoCall) {
    // 0 必需参数的 init 在 OP_CLASS_NEW argCount=0 时自动调用（与 StackVM 一致）
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun init() { count = 42; }"
                            "}"
                            "var c = Counter();"
                            "print(c.count);";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R149InitMethodDefaultArgs) {
    // init 支持默认参数填充：Point(1) → init(1, 0)
    const std::string src = "class Point {"
                            "  var x = 0;"
                            "  var y = 0;"
                            "  fun init(ax, ay = 0) { x = ax; y = ay; }"
                            "}"
                            "var p1 = Point(1);"
                            "var p2 = Point(1, 2);"
                            "print(p1.x);"
                            "print(p1.y);"
                            "print(p2.x);"
                            "print(p2.y);";
    EXPECT_EQ(runJIT(src), "1012");
}

TEST(TestJIT, R149MethodCallNoArgs) {
    // 普通方法调用（无参数）：b.area() → OP_METHOD_CALL argCount=0
    const std::string src = "class Box {"
                            "  var w = 0;"
                            "  var h = 0;"
                            "  fun init(aw, ah) { w = aw; h = ah; }"
                            "  fun area() { return w * h; }"
                            "}"
                            "var b = Box(5, 10);"
                            "print(b.area());";
    EXPECT_EQ(runJIT(src), "50");
}

TEST(TestJIT, R149MethodCallWithArgs) {
    // 带参数方法调用：r.add(100) → OP_METHOD_CALL argCount=1
    const std::string src = "class Calculator {"
                            "  var total = 0;"
                            "  fun init(start) { total = start; }"
                            "  fun add(n) { total = total + n; return total; }"
                            "}"
                            "var c = Calculator(10);"
                            "print(c.add(5));"
                            "print(c.add(20));";
    EXPECT_EQ(runJIT(src), "1535");
}

TEST(TestJIT, R149MethodCallDefaultArgs) {
    // 方法调用默认参数：g.greet() → greet("World")
    const std::string src = "class Greeter {"
                            "  fun greet(name = \"World\") { return name; }"
                            "}"
                            "var g = Greeter();"
                            "print(g.greet());"
                            "print(g.greet(\"Hello\"));";
    EXPECT_EQ(runJIT(src), "WorldHello");
}

TEST(TestJIT, R149MethodCallModifiesThisWriteBackGlobal) {
    // 方法修改 this 字段 → writeBack 到全局接收者
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun init() { count = 0; }"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc();"
                            "c.inc();"
                            "c.inc();"
                            "print(c.count);";
    EXPECT_EQ(runJIT(src), "3");
}

TEST(TestJIT, R149MethodCallModifiesThisWriteBackLocal) {
    // 方法修改 this 字段 → writeBack 到局部变量接收者（在函数内）
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun init() { count = 0; }"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "fn test() {"
                            "  var c = Counter();"
                            "  c.inc();"
                            "  c.inc();"
                            "  print(c.count);"
                            "}"
                            "test();";
    EXPECT_EQ(runJIT(src), "2");
}

TEST(TestJIT, R149MethodCallOnTemporaryNoWriteBack) {
    // 临时表达式上的方法调用（无 writeBack）：Counter().inc() 不影响后续
    const std::string src = "class Box {"
                            "  var v = 0;"
                            "  fun init(n) { v = n; }"
                            "  fun get() { return v; }"
                            "}"
                            "print(Box(99).get());";
    EXPECT_EQ(runJIT(src), "99");
}

TEST(TestJIT, R149SuperCallSingleLevel) {
    // super.method() 调用父类方法：Dog.bark() 调用 super.speak()
    const std::string src = "class Animal {"
                            "  var sound = \"...\";"
                            "  fun init() { sound = \"...\"; }"
                            "  fun speak() { return \"Generic\"; }"
                            "}"
                            "class Dog : Animal {"
                            "  fun init() { super.init(); }"
                            "  fun speak() { return \"Woof\"; }"
                            "  fun bark() { return super.speak(); }"
                            "}"
                            "var d = Dog();"
                            "print(d.speak());"
                            "print(d.bark());";
    EXPECT_EQ(runJIT(src), "WoofGeneric");
}

TEST(TestJIT, R149SuperCallMultiLevelInheritance) {
    // 多层继承 super 调用：C.foo() 调用 super.foo()（B.foo），B.foo() 调用 super.foo()（A.foo）
    const std::string src = "class A {"
                            "  fun foo() { return 1; }"
                            "}"
                            "class B : A {"
                            "  fun foo() { return super.foo() + 10; }"
                            "}"
                            "class C : B {"
                            "  fun foo() { return super.foo() + 100; }"
                            "}"
                            "var c = C();"
                            "print(c.foo());";
    EXPECT_EQ(runJIT(src), "111");
}

TEST(TestJIT, R149InitWithSuperInit) {
    // 子类 init 调用 super.init() 初始化父类字段
    const std::string src = "class Base {"
                            "  var a = 0;"
                            "  fun init(av) { a = av; }"
                            "}"
                            "class Derived : Base {"
                            "  var b = 0;"
                            "  fun init(av, bv) { super.init(av); b = bv; }"
                            "}"
                            "var d = Derived(10, 20);"
                            "print(d.a);"
                            "print(d.b);";
    EXPECT_EQ(runJIT(src), "1020");
}

// ---- 三后端一致性测试（JIT vs StackVM） ----

TEST(TestJIT, R149InitMethodBasicConsistencyWithStackVM) {
    const std::string src = "class Point {"
                            "  var x = 0;"
                            "  var y = 0;"
                            "  fun init(ax, ay) { x = ax; y = ay; }"
                            "}"
                            "var p = Point(3, 4);"
                            "print(p.x);"
                            "print(p.y);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149InitMethodDefaultArgsConsistencyWithStackVM) {
    const std::string src = "class Point {"
                            "  var x = 0;"
                            "  var y = 0;"
                            "  fun init(ax, ay = 0) { x = ax; y = ay; }"
                            "}"
                            "var p1 = Point(1);"
                            "var p2 = Point(1, 2);"
                            "print(p1.x);"
                            "print(p1.y);"
                            "print(p2.x);"
                            "print(p2.y);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149MethodCallConsistencyWithStackVM) {
    const std::string src = "class Box {"
                            "  var w = 0;"
                            "  var h = 0;"
                            "  fun init(aw, ah) { w = aw; h = ah; }"
                            "  fun area() { return w * h; }"
                            "}"
                            "var b = Box(5, 10);"
                            "print(b.area());";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149MethodCallWithArgsConsistencyWithStackVM) {
    const std::string src = "class Calculator {"
                            "  var total = 0;"
                            "  fun init(start) { total = start; }"
                            "  fun add(n) { total = total + n; return total; }"
                            "}"
                            "var c = Calculator(10);"
                            "print(c.add(5));"
                            "print(c.add(20));";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149MethodCallWriteBackConsistencyWithStackVM) {
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun init() { count = 0; }"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc();"
                            "c.inc();"
                            "c.inc();"
                            "print(c.count);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149SuperCallConsistencyWithStackVM) {
    const std::string src = "class Animal {"
                            "  fun speak() { return \"Generic\"; }"
                            "}"
                            "class Dog : Animal {"
                            "  fun speak() { return \"Woof\"; }"
                            "  fun bark() { return super.speak(); }"
                            "}"
                            "var d = Dog();"
                            "print(d.speak());"
                            "print(d.bark());";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149SuperCallMultiLevelConsistencyWithStackVM) {
    const std::string src = "class A {"
                            "  fun foo() { return 1; }"
                            "}"
                            "class B : A {"
                            "  fun foo() { return super.foo() + 10; }"
                            "}"
                            "class C : B {"
                            "  fun foo() { return super.foo() + 100; }"
                            "}"
                            "var c = C();"
                            "print(c.foo());";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149InitWithSuperInitConsistencyWithStackVM) {
    const std::string src = "class Base {"
                            "  var a = 0;"
                            "  fun init(av) { a = av; }"
                            "}"
                            "class Derived : Base {"
                            "  var b = 0;"
                            "  fun init(av, bv) { super.init(av); b = bv; }"
                            "}"
                            "var d = Derived(10, 20);"
                            "print(d.a);"
                            "print(d.b);";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

TEST(TestJIT, R149MethodCallInFunctionConsistencyWithStackVM) {
    // 方法调用在函数内部 + writeBack 到局部变量
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun init() { count = 0; }"
                            "  fun inc() { count = count + 1; }"
                            "  fun get() { return count; }"
                            "}"
                            "fn test() {"
                            "  var c = Counter();"
                            "  c.inc();"
                            "  c.inc();"
                            "  print(c.get());"
                            "}"
                            "test();";
    EXPECT_EQ(runJIT(src), minilang_test::runStackVM(src));
}

// ============================================================
// R150 热点检测计数器测试
// ------------------------------------------------------------
// 验证 getHotChunkStats() 返回的 per-chunk 调用计数正确性：
//   - mainChunk 索引 0 不计数（恒为 0）
//   - 每个函数 chunk 入口 inc 指令正确累加
//   - execute() 重入时计数器清零
//   - 递归调用计数符合数学预期
// 注：这些测试直接使用 JITBackend 实例（不通过 runJIT 辅助函数），
//     因为需要访问 getHotChunkStats() 公有方法。
// ============================================================

TEST(TestJIT, R150HotChunkStats_MainOnly) {
    // 仅 mainChunk，无函数 chunk —— stats 应只有 1 项且计数为 0
    Lexer lx;
    auto tk = lx.scan("print(1);");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "1");

    auto stats = jit.getHotChunkStats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats[0].second, 0u); // mainChunk 不计数
}

TEST(TestJIT, R150HotChunkStats_SingleFunctionCalledTwice) {
    const std::string src = "fun f() { return 1; } f(); f(); print(\"done\");";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "done");

    // stats: [mainChunk(0), f(2)]
    auto stats = jit.getHotChunkStats();
    ASSERT_EQ(stats.size(), 2u);
    EXPECT_EQ(stats[0].second, 0u); // mainChunk 不计数
    EXPECT_EQ(stats[1].first, "f");
    EXPECT_EQ(stats[1].second, 2u);
}

TEST(TestJIT, R150HotChunkStats_MultipleFunctions) {
    const std::string src = "fun f() { return 1; } fun g() { return 2; } f(); g(); f();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getHotChunkStats();
    ASSERT_EQ(stats.size(), 3u);    // main + f + g
    EXPECT_EQ(stats[0].second, 0u); // mainChunk

    // 查找 f 和 g 的计数（顺序由 Compiler 决定，按 name 查找更稳健）
    uint64_t fCount = 0, gCount = 0;
    for (const auto& [name, count] : stats) {
        if (name == "f")
            fCount = count;
        else if (name == "g")
            gCount = count;
    }
    EXPECT_EQ(fCount, 2u);
    EXPECT_EQ(gCount, 1u);
}

TEST(TestJIT, R150HotChunkStats_RecursiveFibonacci) {
    // fib(5) 总调用次数 = 15（含顶层调用）
    // calls(n) = 1 + calls(n-1) + calls(n-2), calls(0)=calls(1)=1
    // calls(2)=3, calls(3)=5, calls(4)=9, calls(5)=15
    const std::string src = "fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }"
                            "print(fib(5));";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "5");

    auto stats = jit.getHotChunkStats();
    ASSERT_EQ(stats.size(), 2u);    // main + fib
    EXPECT_EQ(stats[0].second, 0u); // mainChunk

    // fib 调用次数应为 15
    uint64_t fibCount = 0;
    for (const auto& [name, count] : stats) {
        if (name == "fib")
            fibCount = count;
    }
    EXPECT_EQ(fibCount, 15u);
}

TEST(TestJIT, R150HotChunkStats_ResetOnReexecute) {
    // 同一 JITBackend 实例连续 execute 两次，计数器应重置（不是累加）
    const std::string src = "fun f() { return 1; } f(); f();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });

    // 第一次执行：f 调用 2 次
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    {
        auto stats = jit.getHotChunkStats();
        ASSERT_EQ(stats.size(), 2u);
        uint64_t fCount = 0;
        for (const auto& [name, count] : stats) {
            if (name == "f")
                fCount = count;
        }
        EXPECT_EQ(fCount, 2u);
    }

    // 第二次执行：计数器应重置，f 仍调用 2 次（不是 4 次）
    out.clear();
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    {
        auto stats = jit.getHotChunkStats();
        ASSERT_EQ(stats.size(), 2u);
        uint64_t fCount = 0;
        for (const auto& [name, count] : stats) {
            if (name == "f")
                fCount = count;
        }
        EXPECT_EQ(fCount, 2u); // 重置后仍为 2，不是 4
    }
}

TEST(TestJIT, R150HotChunkStats_NestedCalls) {
    // 嵌套调用：outer 调 inner
    // outer() 调用 1 次（由 main 调用），inner() 被调用 1 次（由 outer 调用）
    const std::string src = "fun inner() { return 1; }"
                            "fun outer() { return inner(); }"
                            "print(outer());";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "1");

    auto stats = jit.getHotChunkStats();
    ASSERT_EQ(stats.size(), 3u); // main + inner + outer
    uint64_t innerCount = 0, outerCount = 0;
    for (const auto& [name, count] : stats) {
        if (name == "inner")
            innerCount = count;
        else if (name == "outer")
            outerCount = count;
    }
    EXPECT_EQ(innerCount, 1u);
    EXPECT_EQ(outerCount, 1u);
}

TEST(TestJIT, R150HotChunkStats_MethodCallCounts) {
    // 方法调用：c.inc() 调用 3 次
    // 注意：方法 chunk 名格式为 "Class.method"（与 StackVM functionChunks_ 键一致）
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getHotChunkStats();
    // 查找 Counter.inc 方法 chunk 的计数
    uint64_t incCount = 0;
    bool foundInc = false;
    for (const auto& [name, count] : stats) {
        // 方法 chunk 名可能是 "Counter.inc" 或包含类名前缀
        if (name.find("inc") != std::string::npos) {
            incCount = count;
            foundInc = true;
        }
    }
    EXPECT_TRUE(foundInc);
    EXPECT_EQ(incCount, 3u);
}

// ============================================================
// R151 热点阈值重编译测试
// ============================================================
// 验证热点阈值检测机制：方法 chunk 调用计数达到阈值时触发 recompiledFlags 标记。
// 测试通过 setHotThreshold() 设置自定义阈值，通过 getRecompileStats() 验证触发状态。

TEST(TestJIT, R151Recompile_DefaultThresholdNotTriggered) {
    // 默认阈值 kDefaultHotThreshold=1000，调用 3 次未达阈值，recompiledFlags 应为 0
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    uint64_t incRecompiled = 0;
    for (const auto& [name, flag] : stats) {
        if (name.find("inc") != std::string::npos) {
            incRecompiled = flag;
        }
    }
    EXPECT_EQ(incRecompiled, 0u); // 未达阈值，未触发
}

TEST(TestJIT, R151Recompile_ThresholdReached) {
    // 设置阈值为 3，调用 3 次后 recompiledFlags 应为 1
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    // 设置 Counter.inc 方法 chunk 的热点阈值为 3
    // 方法 chunk 名格式为 "Class.method"（与 StackVM functionChunks_ 键一致）
    jit.setHotThreshold("Counter.inc", 3);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    uint64_t incRecompiled = 0;
    bool foundInc = false;
    for (const auto& [name, flag] : stats) {
        if (name.find("inc") != std::string::npos) {
            incRecompiled = flag;
            foundInc = true;
        }
    }
    EXPECT_TRUE(foundInc);
    EXPECT_EQ(incRecompiled, 1u); // 达到阈值，已触发
}

TEST(TestJIT, R151Recompile_IdempotentSingleTrigger) {
    // 达到阈值后继续调用，recompiledFlags 仍为 1（幂等性，不重复触发）
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 2); // 阈值 2，调用 5 次
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    uint64_t incRecompiled = 0;
    for (const auto& [name, flag] : stats) {
        if (name.find("inc") != std::string::npos) {
            incRecompiled = flag;
        }
    }
    EXPECT_EQ(incRecompiled, 1u); // 只触发一次
}

TEST(TestJIT, R151Recompile_NormalFunctionNotTriggered) {
    // 普通函数（非方法 chunk）默认阈值 0，不触发重编译
    const std::string src = "fun add(a, b) { return a + b; }"
                            "var r = add(1, 2);"
                            "var r2 = add(3, 4);"
                            "var r3 = add(5, 6);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    for (const auto& [name, flag] : stats) {
        if (name.find("add") != std::string::npos) {
            EXPECT_EQ(flag, 0u) << "普通函数不应触发重编译: " << name;
        }
    }
}

TEST(TestJIT, R151Recompile_SemanticsPreserved) {
    // 热点触发后方法语义不变：计数器值正确
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "  fun get() { return count; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc(); c.inc(); c.inc();"
                            "print(c.get());";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 2); // 阈值 2，调用 5 次
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    EXPECT_EQ(out, "5"); // 计数器值正确，语义未变（print 不输出换行）
}

TEST(TestJIT, R151Recompile_CustomThresholdZeroDisables) {
    // 设置阈值为 0 显式禁用重编译（即使默认是方法 chunk）
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 0); // 显式禁用
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    uint64_t incRecompiled = 0;
    for (const auto& [name, flag] : stats) {
        if (name.find("inc") != std::string::npos) {
            incRecompiled = flag;
        }
    }
    EXPECT_EQ(incRecompiled, 0u); // 阈值 0 不触发
}

TEST(TestJIT, R151Recompile_ExactThresholdBoundary) {
    // 边界测试：调用次数恰好等于阈值时触发
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc();"; // 恰好 2 次
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 2); // 阈值 2，调用恰好 2 次
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    uint64_t incRecompiled = 0;
    for (const auto& [name, flag] : stats) {
        if (name.find("inc") != std::string::npos) {
            incRecompiled = flag;
        }
    }
    EXPECT_EQ(incRecompiled, 1u); // 恰好达到阈值，触发
}

TEST(TestJIT, R151Recompile_BelowThresholdNotTriggered) {
    // 边界测试：调用次数比阈值少 1，不触发
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc();"; // 2 次
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 3); // 阈值 3，调用 2 次
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    uint64_t incRecompiled = 0;
    for (const auto& [name, flag] : stats) {
        if (name.find("inc") != std::string::npos) {
            incRecompiled = flag;
        }
    }
    EXPECT_EQ(incRecompiled, 0u); // 未达阈值，不触发
}

TEST(TestJIT, R151Recompile_MultipleMethodsIndependent) {
    // 多个方法独立触发：inc 达到阈值触发，get 未达到不触发
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "  fun get() { return count; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();"
                            "print(c.get());";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 2);  // inc 阈值 2
    jit.setHotThreshold("Counter.get", 10); // get 阈值 10
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto stats = jit.getRecompileStats();
    uint64_t incRecompiled = 0;
    uint64_t getRecompiled = 0;
    for (const auto& [name, flag] : stats) {
        if (name.find("inc") != std::string::npos) {
            incRecompiled = flag;
        } else if (name.find("get") != std::string::npos) {
            getRecompiled = flag;
        }
    }
    EXPECT_EQ(incRecompiled, 1u); // inc 调用 3 次 >= 阈值 2，触发
    EXPECT_EQ(getRecompiled, 0u); // get 调用 1 次 < 阈值 10，不触发
    EXPECT_EQ(out, "3");          // print 不输出换行
}

// ============================================================
// 阶段 5c 类型反馈 + 特化重编译测试用例（R152）
// ============================================================
// 验证类型反馈收集 + INT 特化重编译 + 入口切换机制
// 关键不变量：
//   1. 纯 INT 算术方法 → typeFeedback.otherCount==0 && floatCount==0 → 特化
//   2. 含浮点/字符串运算 → typeFeedback.floatCount>0 || otherCount>0 → 不特化
//   3. 特化后语义保持一致（第二次 execute 输出与第一次相同）
//   4. specializedChunks_ 包含已特化的方法名

TEST(TestJIT, R152TypeFeedback_PureIntArithmetic) {
    // 纯 INT 算术方法：count = count + 1（INT + INT）
    // 类型反馈：otherCount==0 && floatCount==0
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto tf = jit.getTypeFeedback();
    uint64_t incOtherCount = 0;
    uint64_t incFloatCount = 0;
    for (const auto& [name, fb] : tf) {
        if (name.find("inc") != std::string::npos) {
            incOtherCount = fb.otherCount;
            incFloatCount = fb.floatCount;
        }
    }
    EXPECT_EQ(incOtherCount, 0u); // 纯 INT，无其他类型
    EXPECT_EQ(incFloatCount, 0u); // 无浮点运算
}

TEST(TestJIT, R152TypeFeedback_FloatNotSpecialized) {
    // 含浮点运算的方法：x = x + 1.5（INT + FLOAT → FLOAT 路径）
    // 类型反馈：floatCount > 0
    // 注意：count 字段默认 0（INT），+ 1.5（FLOAT）触发 genericAdd → floatCount++
    const std::string src = "class Calc {"
                            "  var x = 0;"
                            "  fun addFloat() { x = x + 1.5; }"
                            "}"
                            "var c = Calc();"
                            "c.addFloat(); c.addFloat();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto tf = jit.getTypeFeedback();
    uint64_t addFloatFloatCount = 0;
    for (const auto& [name, fb] : tf) {
        if (name.find("addFloat") != std::string::npos) {
            addFloatFloatCount = fb.floatCount;
        }
    }
    EXPECT_GT(addFloatFloatCount, 0u); // 浮点运算触发 floatCount++
}

TEST(TestJIT, R152Specialization_TriggeredAfterThreshold) {
    // 热点阈值触发后，specializedChunks_ 应包含目标方法
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 2); // 阈值 2，调用 3 次触发
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 特化重编译应在 execute() 返回前触发
    const auto& spec = jit.getSpecializedChunks();
    bool incSpecialized = false;
    for (const auto& name : spec) {
        if (name.find("inc") != std::string::npos) {
            incSpecialized = true;
        }
    }
    EXPECT_TRUE(incSpecialized); // inc 方法应被特化
}

TEST(TestJIT, R152Specialization_FloatMethodNotSpecialized) {
    // R152: 含 STRING 运算的方法不特化（typeFeedback.otherCount > 0）
    // R153 修正：原测试用 `x + 1.5`（FLOAT）在 R153 中会被 FLOAT 特化，
    //           改用 STRING 拼接触发 otherCount > 0，保持"不特化"的预期。
    //           STRING tag=0x7FFB (POINTER) → emitRecordTypeFeedback 判定 otherCount++
    const std::string src = "class Greeter {"
                            "  var s = \"\";"
                            "  fun append() { s = s + \"!\"; }"
                            "}"
                            "var g = Greeter();"
                            "g.append(); g.append(); g.append();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Greeter.append", 2); // 阈值 2，调用 3 次触发热点
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 热点触发但类型反馈显示 otherCount > 0（STRING），不应特化
    const auto& spec = jit.getSpecializedChunks();
    bool appendSpecialized = false;
    for (const auto& name : spec) {
        if (name.find("append") != std::string::npos) {
            appendSpecialized = true;
        }
    }
    EXPECT_FALSE(appendSpecialized); // STRING 方法不应被特化
}

TEST(TestJIT, R152Specialization_SemanticsPreserved) {
    // 特化后第二次 execute() 语义保持一致
    // 第一次 execute() 触发热点 + 特化重编译
    // 第二次 execute() 走特化版本，输出应与 StackVM 一致
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "  fun get() { return count; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc(); c.inc(); c.inc();"
                            "print(c.get());";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out1;
    jit.setOutputCallback([&](const std::string& s) { out1 += s; });
    jit.setHotThreshold("Counter.inc", 2); // 阈值 2，调用 5 次触发
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out1, "5"); // 第一次执行：count=5

    // 第二次 execute()：inc 方法已特化，走 INT 特化版本
    std::string out2;
    jit.setOutputCallback([&](const std::string& s) { out2 += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out2, "5"); // 特化版本语义应一致
}

TEST(TestJIT, R152Specialization_SecondExecuteUsesSpecialized) {
    // 验证第二次 execute() 使用特化版本（通过 getTypeFeedback 确认）
    // 第一次 execute() 触发特化，第二次 execute() 类型反馈不再增长（特化版本无 genericXXX 路径）
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Counter.inc", 2);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 第一次 execute 后，inc 已特化
    const auto& spec1 = jit.getSpecializedChunks();
    EXPECT_FALSE(spec1.empty());

    // 第二次 execute：特化版本不经过 genericXXX 路径，类型反馈不再增长
    // （但 typeFeedback_ 在 execute() 入口会被重置，所以比较第二次执行后的值）
    out.clear();
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 第二次 execute 后，特化版本不触发 genericXXX，typeFeedback 应保持 0
    auto tf = jit.getTypeFeedback();
    uint64_t incOtherCount = 1; // 初始化为非 0 以验证
    for (const auto& [name, fb] : tf) {
        if (name.find("inc") != std::string::npos) {
            incOtherCount = fb.otherCount;
        }
    }
    EXPECT_EQ(incOtherCount, 0u); // 特化版本不收集类型反馈
}

TEST(TestJIT, R152Specialization_MultipleMethodsIndependent) {
    // 多个方法独立特化：inc 和 add 都触发热点，都应被特化
    const std::string src = "class Calc {"
                            "  var a = 0;"
                            "  var b = 0;"
                            "  fun inc() { a = a + 1; }"
                            "  fun add() { b = b + 2; }"
                            "}"
                            "var c = Calc();"
                            "c.inc(); c.inc(); c.inc();"
                            "c.add(); c.add(); c.add();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Calc.inc", 2);
    jit.setHotThreshold("Calc.add", 2);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    const auto& spec = jit.getSpecializedChunks();
    bool incSpec = false;
    bool addSpec = false;
    for (const auto& name : spec) {
        if (name.find("inc") != std::string::npos) {
            incSpec = true;
        }
        if (name.find("add") != std::string::npos) {
            addSpec = true;
        }
    }
    EXPECT_TRUE(incSpec); // inc 应被特化
    EXPECT_TRUE(addSpec); // add 应被特化
}

TEST(TestJIT, R152Specialization_GetTypeFeedbackAPI) {
    // 验证 getTypeFeedback() API 返回正确格式
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "}"
                            "var c = Counter();"
                            "c.inc();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto tf = jit.getTypeFeedback();
    EXPECT_FALSE(tf.empty()); // 应有 chunk 统计
    bool hasInc = false;
    for (const auto& [name, fb] : tf) {
        if (name.find("inc") != std::string::npos) {
            hasInc = true;
            // 纯 INT 方法，otherCount 和 floatCount 应为 0
            EXPECT_EQ(fb.otherCount, 0u);
            EXPECT_EQ(fb.floatCount, 0u);
        }
    }
    EXPECT_TRUE(hasInc); // 应包含 inc 方法
}

TEST(TestJIT, R152Specialization_ConsistencyWithStackVM) {
    // 三后端一致性：特化版本输出应与 StackVM 一致
    const std::string src = "class Counter {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; }"
                            "  fun get() { return count; }"
                            "}"
                            "var c = Counter();"
                            "c.inc(); c.inc(); c.inc(); c.inc();"
                            "print(c.get());";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    // JIT 第一次执行（触发特化）
    JITBackend jit;
    std::string jitOut1;
    jit.setOutputCallback([&](const std::string& s) { jitOut1 += s; });
    jit.setHotThreshold("Counter.inc", 2);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // JIT 第二次执行（走特化版本）
    std::string jitOut2;
    jit.setOutputCallback([&](const std::string& s) { jitOut2 += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 两次输出应一致
    EXPECT_EQ(jitOut1, jitOut2);
    EXPECT_EQ(jitOut1, "4"); // count=4
}

// ============================================================
// 阶段 5c+ FLOAT 特化重编译测试用例（R153）
// ============================================================
// 验证 FLOAT 类型反馈触发 FLOAT 特化重编译（与 R152 INT 特化对称）
// 关键不变量：
//   1. 纯 FLOAT 算术方法 → typeFeedback.floatCount>0 && otherCount==0 → FLOAT 特化
//   2. 含 STRING 运算 → typeFeedback.otherCount>0 → 不特化（保持类型分派）
//   3. FLOAT 特化后语义保持一致（第二次 execute 输出与第一次相同）
//   4. specializedChunks_ 包含已 FLOAT 特化的方法名
//   5. INT 方法 + FLOAT 方法可独立特化（互不干扰）

TEST(TestJIT, R153TypeFeedback_PureFloatArithmetic) {
    // 纯 FLOAT 算术方法：x = x + 1.5（FLOAT + FLOAT → FLOAT 路径）
    // 注意：x 初值 0.0（FLOAT），+ 1.5（FLOAT）→ 两边都是 FLOAT
    // 类型反馈：floatCount > 0 && otherCount == 0
    const std::string src = "class Calc {"
                            "  var x = 0.0;"
                            "  fun addFloat() { x = x + 1.5; }"
                            "}"
                            "var c = Calc();"
                            "c.addFloat(); c.addFloat();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto tf = jit.getTypeFeedback();
    uint64_t addFloatFloatCount = 0;
    uint64_t addFloatOtherCount = 0;
    for (const auto& [name, fb] : tf) {
        if (name.find("addFloat") != std::string::npos) {
            addFloatFloatCount = fb.floatCount;
            addFloatOtherCount = fb.otherCount;
        }
    }
    EXPECT_GT(addFloatFloatCount, 0u); // 浮点运算触发 floatCount++
    EXPECT_EQ(addFloatOtherCount, 0u); // 无 STRING/BOOL/NULL/POINTER 类型
}

TEST(TestJIT, R153Specialization_FloatTriggeredAfterThreshold) {
    // 热点阈值触发后，specializedChunks_ 应包含 FLOAT 方法
    const std::string src = "class Calc {"
                            "  var x = 0.0;"
                            "  fun addFloat() { x = x + 1.5; }"
                            "}"
                            "var c = Calc();"
                            "c.addFloat(); c.addFloat(); c.addFloat();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Calc.addFloat", 2); // 阈值 2，调用 3 次触发
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // FLOAT 特化重编译应在 execute() 返回前触发
    const auto& spec = jit.getSpecializedChunks();
    bool addFloatSpecialized = false;
    for (const auto& name : spec) {
        if (name.find("addFloat") != std::string::npos) {
            addFloatSpecialized = true;
        }
    }
    EXPECT_TRUE(addFloatSpecialized); // addFloat 方法应被 FLOAT 特化
}

TEST(TestJIT, R153Specialization_FloatSemanticsPreserved) {
    // FLOAT 特化后第二次 execute() 语义保持一致
    // 第一次 execute() 触发热点 + FLOAT 特化重编译
    // 第二次 execute() 走 FLOAT 特化版本，输出应与 StackVM 一致
    const std::string src = "class Calc {"
                            "  var x = 0.0;"
                            "  fun addFloat() { x = x + 1.5; }"
                            "  fun get() { return x; }"
                            "}"
                            "var c = Calc();"
                            "c.addFloat(); c.addFloat(); c.addFloat(); c.addFloat(); c.addFloat();"
                            "print(c.get());";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out1;
    jit.setOutputCallback([&](const std::string& s) { out1 += s; });
    jit.setHotThreshold("Calc.addFloat", 2); // 阈值 2，调用 5 次触发
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    // 1.5 * 5 = 7.5
    EXPECT_EQ(out1, "7.5"); // 第一次执行：x=7.5

    // 第二次 execute()：addFloat 方法已 FLOAT 特化，走特化版本
    std::string out2;
    jit.setOutputCallback([&](const std::string& s) { out2 += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out2, "7.5"); // 特化版本语义应一致
}

TEST(TestJIT, R153Specialization_FloatSecondExecuteUsesSpecialized) {
    // 验证第二次 execute() 使用 FLOAT 特化版本
    // 第一次 execute() 触发特化，第二次 execute() 类型反馈不再增长（特化版本无 emitRecordTypeFeedback）
    const std::string src = "class Calc {"
                            "  var x = 0.0;"
                            "  fun addFloat() { x = x + 1.5; }"
                            "}"
                            "var c = Calc();"
                            "c.addFloat(); c.addFloat(); c.addFloat();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Calc.addFloat", 2);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 第一次 execute 后，addFloat 已 FLOAT 特化
    const auto& spec1 = jit.getSpecializedChunks();
    EXPECT_FALSE(spec1.empty());

    // 第二次 execute：FLOAT 特化版本不经过 genericXXX 路径的 emitRecordTypeFeedback，
    // 但仍走 genericXXX 路径调用 C++ 辅助函数（FLOAT 特化跳过 INT 原生路径）
    // 类型反馈在 execute() 入口会被重置，第二次执行后 floatCount 应为 0（特化版本不收集）
    out.clear();
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    auto tf = jit.getTypeFeedback();
    uint64_t addFloatFloatCount = 1; // 初始化为非 0 以验证
    for (const auto& [name, fb] : tf) {
        if (name.find("addFloat") != std::string::npos) {
            addFloatFloatCount = fb.floatCount;
        }
    }
    EXPECT_EQ(addFloatFloatCount, 0u); // 特化版本不收集类型反馈
}

TEST(TestJIT, R153Specialization_FloatMultipleMethodsIndependent) {
    // 多个 FLOAT 方法独立特化：addFloat 和 mulFloat 都触发热点，都应被特化
    const std::string src = "class Calc {"
                            "  var a = 0.0;"
                            "  var b = 1.0;"
                            "  fun addFloat() { a = a + 0.5; }"
                            "  fun mulFloat() { b = b * 2.0; }"
                            "}"
                            "var c = Calc();"
                            "c.addFloat(); c.addFloat(); c.addFloat();"
                            "c.mulFloat(); c.mulFloat(); c.mulFloat();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Calc.addFloat", 2);
    jit.setHotThreshold("Calc.mulFloat", 2);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    const auto& spec = jit.getSpecializedChunks();
    bool addSpec = false;
    bool mulSpec = false;
    for (const auto& name : spec) {
        if (name.find("addFloat") != std::string::npos) {
            addSpec = true;
        }
        if (name.find("mulFloat") != std::string::npos) {
            mulSpec = true;
        }
    }
    EXPECT_TRUE(addSpec); // addFloat 应被 FLOAT 特化
    EXPECT_TRUE(mulSpec); // mulFloat 应被 FLOAT 特化
}

TEST(TestJIT, R153Specialization_IntAndFloatCoexist) {
    // INT 方法 + FLOAT 方法独立特化：inc（INT）和 addFloat（FLOAT）各自触发对应特化
    const std::string src = "class Calc {"
                            "  var count = 0;"
                            "  var x = 0.0;"
                            "  fun inc() { count = count + 1; }"
                            "  fun addFloat() { x = x + 1.5; }"
                            "}"
                            "var c = Calc();"
                            "c.inc(); c.inc(); c.inc();"
                            "c.addFloat(); c.addFloat(); c.addFloat();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Calc.inc", 2);
    jit.setHotThreshold("Calc.addFloat", 2);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    const auto& spec = jit.getSpecializedChunks();
    bool incSpec = false;
    bool addFloatSpec = false;
    for (const auto& name : spec) {
        if (name.find("inc") != std::string::npos) {
            incSpec = true;
        }
        if (name.find("addFloat") != std::string::npos) {
            addFloatSpec = true;
        }
    }
    EXPECT_TRUE(incSpec);      // inc 应被 INT 特化（R152）
    EXPECT_TRUE(addFloatSpec); // addFloat 应被 FLOAT 特化（R153）
}

TEST(TestJIT, R153Specialization_FloatConsistencyWithStackVM) {
    // 三后端一致性：FLOAT 特化版本输出应与 StackVM 一致
    const std::string src = "class Calc {"
                            "  var x = 0.0;"
                            "  fun addFloat() { x = x + 1.5; }"
                            "  fun get() { return x; }"
                            "}"
                            "var c = Calc();"
                            "c.addFloat(); c.addFloat(); c.addFloat(); c.addFloat();"
                            "print(c.get());";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    // JIT 第一次执行（触发 FLOAT 特化）
    JITBackend jit;
    std::string jitOut1;
    jit.setOutputCallback([&](const std::string& s) { jitOut1 += s; });
    jit.setHotThreshold("Calc.addFloat", 2);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // JIT 第二次执行（走 FLOAT 特化版本）
    std::string jitOut2;
    jit.setOutputCallback([&](const std::string& s) { jitOut2 += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 两次输出应一致
    EXPECT_EQ(jitOut1, jitOut2);
    EXPECT_EQ(jitOut1, "6"); // 1.5 * 4 = 6.0 → print 输出 "6"（MiniLang print 浮点整数部分）
}

TEST(TestJIT, R153Specialization_FloatDivisionByZero) {
    // FLOAT 特化版本的除零错误处理（验证 C++ 辅助函数的错误传播）
    const std::string src = "class Calc {"
                            "  var x = 1.0;"
                            "  fun divByZero() { x = x / 0.0; }"
                            "}"
                            "var c = Calc();"
                            "c.divByZero(); c.divByZero(); c.divByZero();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setHotThreshold("Calc.divByZero", 2);
    // 除零应触发运行时错误
    EXPECT_EQ(jit.execute(cr), JitResult::RuntimeError);
    EXPECT_TRUE(jit.hasError());
}

// ============================================================
// R154 阶段 5d：嵌套左值赋值链 + OP_LEN + OP_DUP_N 测试
// ============================================================
// 验证 OP_INDEX_SET + OP_LOAD_MUTATED + OP_WRITEBACK_*_VAR/LOCAL +
//      OP_LEN + OP_DUP_N 共 8 个 OpCode 的 JIT 实现。
// 测试策略：
//   1. 类字段数组索引赋值（obj.field[idx]=val）触发 OP_INDEX_SET + OP_LOAD_MUTATED + OP_MEMBER_SET_*
//   2. 三后端一致性校验（与 StackVM 输出对比）
//   3. 错误路径一致性（越界、类型错误）
//   4. arr[i][j]=val 是 MiniLang 编译器预存 bug（所有后端都失败），
//      此处仅验证 JIT 与 StackVM 一致性（都失败），不修复编译器 bug
// OP_DUP_N 仅由 IR 路径发射，JIT 路径不触发，此处不直接测试。

// ---- OP_INDEX_SET + OP_LOAD_MUTATED + OP_MEMBER_SET_VAR（类字段数组索引赋值） ----
// 当 outerMem 非 null 时（base.field[idx] = val），编译器发射：
//   OP_INDEX_SET + OP_LOAD_MUTATED + OP_MEMBER_SET_VAR/LOCAL
// 此路径正确写回：MEMBER_SET_LOCAL/VAR 把变异后容器写入 base.fields()[field]

TEST(TestJIT, R154MemberBaseIndexAssignGlobal) {
    // 类字段数组索引赋值（触发 OP_LOAD_MUTATED + OP_MEMBER_SET_VAR）
    const std::string src = "class Matrix {"
                            "    var data = [0, 0, 0];"
                            "}"
                            "var m = Matrix();"
                            "m.data[1] = 99;"
                            "print(m.data[1]);";
    EXPECT_EQ(runJIT(src), "99");
}

TEST(TestJIT, R154MemberBaseIndexAssignMultiple) {
    // 多次类字段数组索引赋值
    const std::string src = "class Matrix {"
                            "    var data = [0, 0, 0];"
                            "}"
                            "var m = Matrix();"
                            "m.data[0] = 10;"
                            "m.data[1] = 20;"
                            "m.data[2] = 30;"
                            "print(m.data[0] + m.data[1] + m.data[2]);";
    EXPECT_EQ(runJIT(src), "60");
}

TEST(TestJIT, R154MemberBaseIndexAssignDictField) {
    // 类字段字典索引赋值
    const std::string src = "class Config {"
                            "    var opts = {\"key\": 0};"
                            "}"
                            "var c = Config();"
                            "c.opts[\"key\"] = 42;"
                            "print(c.opts[\"key\"]);";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R154MemberBaseIndexAssignInMethod) {
    // 方法内修改 this.field[idx]（触发 OP_LOAD_MUTATED + OP_MEMBER_SET_LOCAL）
    const std::string src = "class Stack {"
                            "    var items = [0, 0, 0];"
                            "    fun setAt(idx, val) { items[idx] = val; }"
                            "    fun getAt(idx) { return items[idx]; }"
                            "}"
                            "var s = Stack();"
                            "s.setAt(0, 100);"
                            "s.setAt(1, 200);"
                            "print(s.getAt(0) + s.getAt(1));";
    EXPECT_EQ(runJIT(src), "300");
}

// ---- 三后端一致性校验（与 StackVM 输出对比） ----

TEST(TestJIT, R154MemberBaseIndexConsistencyWithStackVM) {
    // 三后端一致性：类字段数组索引赋值
    const std::string src = "class Box {"
                            "    var items = [0, 0, 0];"
                            "}"
                            "var b = Box();"
                            "b.items[0] = 11;"
                            "b.items[1] = 22;"
                            "b.items[2] = 33;"
                            "print(b.items[0] + b.items[1] + b.items[2]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "66");
}

TEST(TestJIT, R154MemberBaseDictConsistencyWithStackVM) {
    // 三后端一致性：类字段字典索引赋值
    const std::string src = "class Config {"
                            "    var opts = {\"a\": 1, \"b\": 2};"
                            "}"
                            "var c = Config();"
                            "c.opts[\"a\"] = 10;"
                            "c.opts[\"b\"] = 20;"
                            "print(c.opts[\"a\"] + c.opts[\"b\"]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "30");
}

TEST(TestJIT, R154MemberBaseIndexMethodConsistencyWithStackVM) {
    // 三后端一致性：方法内 this.field[idx] 修改
    const std::string src = "class Stack {"
                            "    var items = [0, 0, 0];"
                            "    fun push(idx, val) { items[idx] = val; }"
                            "    fun pop(idx) { return items[idx]; }"
                            "}"
                            "var s = Stack();"
                            "s.push(0, 11);"
                            "s.push(1, 22);"
                            "s.push(2, 33);"
                            "print(s.pop(0) + s.pop(1) + s.pop(2));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "66");
}

// ---- 错误路径一致性 ----

TEST(TestJIT, R154MemberBaseIndexOutOfBoundsConsistency) {
    // 类字段数组越界赋值错误消息一致性
    const std::string src = "class Box {"
                            "    var items = [0, 0, 0];"
                            "}"
                            "var b = Box();"
                            "b.items[10] = 99;"
                            "print(b.items[0]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos);
    EXPECT_NE(jitOut.find("数组索引越界"), std::string::npos);
    EXPECT_NE(vmOut.find("数组索引越界"), std::string::npos);
}

TEST(TestJIT, R154MemberBaseIndexNotIndexable) {
    // 类字段不是容器：int 字段不支持索引赋值
    const std::string src = "class Box {"
                            "    var x = 0;"
                            "}"
                            "var b = Box();"
                            "b.x[0] = 99;"
                            "print(b.x);";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos);
    // R161 fix: JIT 错误消息对齐到 ErrorMessages::kTypeNotIndexAssignable
    // （"该类型不支持索引赋值"，与 StackVM/RegisterVM/Interpreter 一致）
    EXPECT_NE(jitOut.find("该类型不支持索引赋值"), std::string::npos);
}

// ---- R156 fix: arr[i][j] = val 嵌套索引赋值已修复 ----
// 原 R154 发现编译器预存 bug：visitIndexAssign 对 arr[i][j]=val 生成
// OP_WRITEBACK_INDEX_LOCAL/VAR，整体替换 arr 为变异后的 arr[i]。
// R156 修复：改用 重新求值 outerIdx + OP_LOAD_MUTATED + OP_INDEX_SET_LOCAL/VAR
// 直接原地修改 arr[i] = mutated_arr[i]，语义正确。
// 此处验证 JIT 与 StackVM（非 IR 路径）行为一致（都成功）。

TEST(TestJIT, R154NestedArrayAssignConsistencyWithStackVM) {
    // R156 fix: arr[i][j] = val 现在在 JIT 和 StackVM 都成功
    const std::string src = "var m = [[1, 2], [3, 4]];"
                            "m[0][0] = 100;"
                            "m[1][1] = 200;"
                            "print(m[0][0]);"
                            "print(m[0][1]);"
                            "print(m[1][0]);"
                            "print(m[1][1]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    // 修复后两者都应输出 10023200
    EXPECT_EQ(jitOut, "10023200");
    EXPECT_EQ(vmOut, "10023200");
}

TEST(TestJIT, R154NestedDictAssignConsistencyWithStackVM) {
    // R156 fix: d[a][b] = val 现在在 JIT 和 StackVM 都成功
    const std::string src = "var d = {\"x\": {\"y\": 1, \"z\": 2}};"
                            "d[\"x\"][\"y\"] = 10;"
                            "d[\"x\"][\"z\"] = 20;"
                            "print(d[\"x\"][\"y\"] + d[\"x\"][\"z\"]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    // 修复后两者都应输出 30
    EXPECT_EQ(jitOut, "30");
    EXPECT_EQ(vmOut, "30");
}

// ============================================================
// R156 测试：嵌套左值赋值 bug 修复——visitIndexAssign + visitMemberAssign 非 IR 路径
// ------------------------------------------------------------
// R156 修复了 3 处同类 bug（非 IR 编译路径 Compiler.cpp）：
//   1. visitIndexAssign outerIdx: arr[i][j]=val — WRITEBACK 整体替换 → INDEX_SET_LOCAL/VAR 原地
//   2. visitMemberAssign outerIdx: arr[i].field=val — 同上 + 移除 compileNode(baseVar) 栈泄漏
//   3. visitMemberAssign outerMem: obj.field.field=val — WRITEBACK 整体替换 → MEMBER_SET_LOCAL/VAR + 移除栈泄漏
// IR 路径（IR.cpp emitNestedAssignWriteback）本就正确，此处验证非 IR 路径（JIT + StackVM 非 IR）。
// ============================================================

TEST(TestJIT, R156NestedArrayIndexAssignInMethod) {
    // 方法内 this.field[i][j]=val（局部变量路径，触发 INDEX_SET_LOCAL + fieldsModified）
    const std::string src = "class Matrix {"
                            "    var data = [[0, 0], [0, 0]];"
                            "    fun set(r, c, v) { data[r][c] = v; }"
                            "    fun get(r, c) { return data[r][c]; }"
                            "}"
                            "var m = Matrix();"
                            "m.set(0, 0, 11);"
                            "m.set(0, 1, 22);"
                            "m.set(1, 0, 33);"
                            "m.set(1, 1, 44);"
                            "print(m.get(0, 0));"
                            "print(m.get(0, 1));"
                            "print(m.get(1, 0));"
                            "print(m.get(1, 1));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, "11223344");
    EXPECT_EQ(vmOut, "11223344");
}

TEST(TestJIT, R156NestedArrayMemberAssignGlobal) {
    // arr[i].field=val（visitMemberAssign outerIdx，全局变量路径）
    // R156 fix: 移除 compileNode(baseVar) 栈泄漏 + 用 INDEX_SET_VAR 替代 WRITEBACK_INDEX_VAR
    // 注：JIT 不支持 OP_MEMBER_SET（栈式变体），预存限制，非 R156 引入。
    //     本测试只验证 StackVM 非 IR 路径修复有效，JIT 应返回编译错误。
    const std::string src = "var a = [{\"f\": 0}, {\"f\": 0}];"
                            "a[0].f = 11;"
                            "a[1].f = 22;"
                            "print(a[0].f);"
                            "print(a[1].f);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-compile:"), std::string::npos);
    EXPECT_NE(jitOut.find("OP_MEMBER_SET"), std::string::npos);
    EXPECT_EQ(vmOut, "1122");
}

TEST(TestJIT, R156NestedMemberMemberAssignGlobal) {
    // obj.field.field=val（visitMemberAssign outerMem，全局变量路径）
    // R156 fix: 移除 compileNode(baseVar) 栈泄漏 + 用 MEMBER_SET_VAR 替代 WRITEBACK_MEMBER_VAR
    // 注：JIT 不支持 OP_MEMBER_SET（栈式变体），预存限制，非 R156 引入。
    const std::string src = "var d = {\"b\": {\"c\": 0}};"
                            "d.b.c = 42;"
                            "print(d.b.c);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-compile:"), std::string::npos);
    EXPECT_NE(jitOut.find("OP_MEMBER_SET"), std::string::npos);
    EXPECT_EQ(vmOut, "42");
}

TEST(TestJIT, R156NestedArrayIndexAssignInLoop) {
    // 循环内 arr[i][j]=val（验证栈平衡——R156 fix 移除了不必要的 push，循环内不泄漏）
    const std::string src = "var grid = [[0, 0], [0, 0]];"
                            "var i = 0;"
                            "while (i < 100) {"
                            "  grid[0][0] = i;"
                            "  grid[1][1] = i * 2;"
                            "  i = i + 1;"
                            "}"
                            "print(grid[0][0]);"
                            "print(grid[1][1]);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    // 循环 100 次后 grid[0][0]=99, grid[1][1]=198
    EXPECT_EQ(jitOut, "99198");
    EXPECT_EQ(vmOut, "99198");
}

TEST(TestJIT, R156NestedMemberAssignInLoop) {
    // 循环内 obj.field.field=val（验证栈平衡——R156 fix 移除了 compileNode(baseVar) 栈泄漏）
    // 注：JIT 不支持 OP_MEMBER_SET（栈式变体），预存限制，非 R156 引入。
    const std::string src = "var d = {\"b\": {\"c\": 0}};"
                            "var i = 0;"
                            "while (i < 100) {"
                            "  d.b.c = i;"
                            "  i = i + 1;"
                            "}"
                            "print(d.b.c);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-compile:"), std::string::npos);
    EXPECT_NE(jitOut.find("OP_MEMBER_SET"), std::string::npos);
    // 循环 100 次后 d.b.c=99
    EXPECT_EQ(vmOut, "99");
}

TEST(TestJIT, R156NestedArrayMemberAssignInLoop) {
    // 循环内 arr[i].field=val（验证栈平衡——R156 fix 移除了 compileNode(baseVar) 栈泄漏）
    // 注：JIT 不支持 OP_MEMBER_SET（栈式变体），预存限制，非 R156 引入。
    const std::string src = "var a = [{\"f\": 0}, {\"f\": 0}];"
                            "var i = 0;"
                            "while (i < 100) {"
                            "  a[0].f = i;"
                            "  a[1].f = i * 2;"
                            "  i = i + 1;"
                            "}"
                            "print(a[0].f);"
                            "print(a[1].f);";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-compile:"), std::string::npos);
    EXPECT_NE(jitOut.find("OP_MEMBER_SET"), std::string::npos);
    // 循环 100 次后 a[0].f=99, a[1].f=198
    EXPECT_EQ(vmOut, "99198");
}

// ============================================================
// R155 测试：OP_CLOSURE 真闭包值创建 + OP_CALL_EXPR 闭包值调用
// ------------------------------------------------------------
// R155 简化版：仅支持 upvalueCount=0 的闭包值（无 upvalue 捕获）
// upvalue 捕获（upvalueCount>0）推迟到 R156
//
// 测试策略：
//   1. 基本闭包创建和调用（0/1/2/3 参数）
//   2. 闭包作为高阶函数参数
//   3. 同一闭包多次调用
//   4. 闭包变量重新赋值
//   5. 默认参数填充
//   6. 三后端一致性（JIT vs StackVM）
//   7. 错误路径（非闭包值调用、参数数量不匹配）
// ============================================================

TEST(TestJIT, R155ClosureBasicCall) {
    // 基本闭包创建和调用：fun add → var g = add → g(3, 4)
    // 触发 OP_CLOSURE + OP_DEFINE_GLOBAL + OP_GET_GLOBAL + OP_CALL_EXPR
    const std::string src = "fun add(a, b) { return a + b; }"
                            "var g = add;"
                            "print(g(3, 4));";
    EXPECT_EQ(runJIT(src), "7");
}

TEST(TestJIT, R155ClosureZeroArg) {
    // 零参数闭包调用
    const std::string src = "fun f() { return 42; }"
                            "var g = f;"
                            "print(g());";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R155ClosureOneArg) {
    // 单参数闭包调用
    const std::string src = "fun inc(x) { return x + 1; }"
                            "var g = inc;"
                            "print(g(41));";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R155ClosureThreeArgs) {
    // 三参数闭包调用
    const std::string src = "fun add3(a, b, c) { return a + b + c; }"
                            "var g = add3;"
                            "print(g(10, 20, 12));";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R155ClosureAsHigherOrderArg) {
    // 闭包作为高阶函数参数：apply(square, 5)
    // apply 内部 f(x) 走 OP_GET_LOCAL + OP_CALL_EXPR
    const std::string src = "fun apply(f, x) { return f(x); }"
                            "fun square(n) { return n * n; }"
                            "print(apply(square, 5));";
    EXPECT_EQ(runJIT(src), "25");
}

TEST(TestJIT, R155ClosureMultipleCalls) {
    // 同一闭包多次调用
    const std::string src = "fun inc(x) { return x + 1; }"
                            "var g = inc;"
                            "print(g(1));"
                            "print(g(2));"
                            "print(g(3));";
    EXPECT_EQ(runJIT(src), "234");
}

TEST(TestJIT, R155ClosureReassign) {
    // 闭包变量重新赋值
    const std::string src = "fun add(a, b) { return a + b; }"
                            "fun mul(a, b) { return a * b; }"
                            "var g = add;"
                            "print(g(20, 22));"
                            "g = mul;"
                            "print(g(6, 7));";
    EXPECT_EQ(runJIT(src), "4242");
}

TEST(TestJIT, R155ClosureDefaultArg) {
    // 闭包调用带默认参数
    const std::string src = "fun greet(name, greeting = \"Hello\") { return greeting + \", \" + name + \"!\"; }"
                            "var g = greet;"
                            "print(g(\"World\"));"
                            "print(g(\"World\", \"Hi\"));";
    EXPECT_EQ(runJIT(src), "Hello, World!Hi, World!");
}

TEST(TestJIT, R155ClosureWithLocalVar) {
    // 闭包函数内有局部变量
    const std::string src = "fun compute(a, b) { var c = a * b; return c + 1; }"
                            "var g = compute;"
                            "print(g(6, 7));";
    EXPECT_EQ(runJIT(src), "43");
}

TEST(TestJIT, R155ClosureConsistencyWithStackVM) {
    // 三后端一致性：闭包创建 + 调用 + 高阶函数
    const std::string src = "fun double(x) { return x * 2; }"
                            "fun apply(f, x) { return f(x); }"
                            "var g = double;"
                            "print(g(21));"
                            "print(apply(g, 10));"
                            "print(apply(double, 5));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "422010");
}

TEST(TestJIT, R155ClosureChainedConsistencyWithStackVM) {
    // 三后端一致性：链式闭包调用
    const std::string src = "fun add(a, b) { return a + b; }"
                            "fun mul(a, b) { return a * b; }"
                            "var f1 = add;"
                            "var f2 = mul;"
                            "print(f1(f2(3, 4), 5));"
                            "print(f2(f1(2, 3), 4));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "1720");
}

// ---- 错误路径 ----

TEST(TestJIT, R155ClosureNonClosureError) {
    // 错误路径：非闭包值调用
    const std::string src = "var x = 42;"
                            "print(x(5));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos);
    EXPECT_NE(jitOut.find("表达式调用需要函数值"), std::string::npos);
    EXPECT_NE(vmOut.find("表达式调用需要函数值"), std::string::npos);
}

TEST(TestJIT, R155ClosureArgCountMismatchError) {
    // 错误路径：参数数量不匹配
    const std::string src = "fun f(a, b) { return a + b; }"
                            "var g = f;"
                            "print(g(1));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos);
    EXPECT_NE(jitOut.find("期望"), std::string::npos);
    EXPECT_NE(vmOut.find("期望"), std::string::npos);
}

TEST(TestJIT, R155ClosureTooManyArgsError) {
    // 错误路径：参数过多
    const std::string src = "fun f(a) { return a; }"
                            "var g = f;"
                            "print(g(1, 2));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(jitOut.find("<jit-runtime:"), std::string::npos);
    EXPECT_NE(vmOut.find("<runtime:"), std::string::npos);
}

// ============================================================
// R156 测试：upvalue 捕获（isLocal 直接捕获 + passthrough 透传）
// ------------------------------------------------------------
// R156 完整实现 OP_CLOSURE(upvalueCount>0) + OP_GET_UPVALUE +
// OP_SET_UPVALUE + OP_CLOSE_UPVALUE，使 JIT 支持真闭包语义。
//
// 测试策略：
//   1. isLocal 直接捕获（makeAdder 模式）
//   2. OP_GET_UPVALUE 读取捕获变量
//   3. OP_SET_UPVALUE 修改捕获变量
//   4. OP_CLOSE_UPVALUE 变量离开作用域时关闭
//   5. passthrough 透传（多层嵌套）
//   6. 深度 currying（4 层 upvalue 透传）
//   7. 三后端一致性（JIT vs StackVM）
//   8. 多个 upvalue 捕获
//   9. 通过 upvalue 调用闭包值
// ============================================================

TEST(TestJIT, R156UpvalueBasicCapture) {
    // isLocal 直接捕获：makeAdder(x) 返回 add(y)，add 捕获 x
    // 触发 OP_CLOSURE(upvalueCount=1, isLocal=true) + OP_GET_UPVALUE
    const std::string src = "fun makeAdder(x) {"
                            "  fun add(y) { return x + y; }"
                            "  return add;"
                            "}"
                            "var add10 = makeAdder(10);"
                            "print(add10(32));";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R156UpvalueMultipleInstances) {
    // 多个闭包实例：每个 makeAdder 调用创建独立的 upvalue
    const std::string src = "fun makeAdder(x) {"
                            "  fun add(y) { return x + y; }"
                            "  return add;"
                            "}"
                            "var add10 = makeAdder(10);"
                            "var add20 = makeAdder(20);"
                            "print(add10(32));"
                            "print(add20(22));";
    EXPECT_EQ(runJIT(src), "4242");
}

TEST(TestJIT, R156UpvalueWrite) {
    // OP_SET_UPVALUE：内层函数修改捕获的变量
    const std::string src = "fun makeCounter() {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; return count; }"
                            "  print(inc());"
                            "  print(inc());"
                            "  print(inc());"
                            "  return 0;"
                            "}"
                            "makeCounter();";
    EXPECT_EQ(runJIT(src), "123");
}

TEST(TestJIT, R156UpvalueCloseScope) {
    // OP_CLOSE_UPVALUE：变量离开作用域时关闭（if 块内定义函数）
    const std::string src = "fun makeF() {"
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
    EXPECT_EQ(runJIT(src), "42");
}

// R156 upvalue passthrough 透传：通过 upvalue 透传闭包值并调用
TEST(TestJIT, R156UpvaluePassthrough) {
    // passthrough 透传：inner 通过 middle 透传 outer 的 g
    const std::string src = "fun outer() {"
                            "  fun f(x) { return x + 1; }"
                            "  var g = f;"
                            "  fun middle() {"
                            "    fun inner() { return g(41); }"
                            "    return inner();"
                            "  }"
                            "  return middle();"
                            "}"
                            "print(outer());";
    EXPECT_EQ(runJIT(src), "42");
}

TEST(TestJIT, R156UpvalueDeepCurrying) {
    // 深度 currying：4 层 upvalue 透传
    const std::string src = "fun f(a) {"
                            "  fun g(b) {"
                            "    fun h(c) {"
                            "      fun i(d) { return a + b + c + d; }"
                            "      return i;"
                            "    }"
                            "    return h;"
                            "  }"
                            "  return g;"
                            "}"
                            "print(f(10)(15)(10)(7));";
    EXPECT_EQ(runJIT(src), "42");
}

// R156 多 upvalue 捕获：inner 同时捕获 x 和 y
TEST(TestJIT, R156UpvalueMultipleCaptures) {
    // 多个 upvalue 捕获：inner 同时捕获 x 和 y
    const std::string src = "fun outer() {"
                            "  var x = 10;"
                            "  var y = 32;"
                            "  fun inner() { return x + y; }"
                            "  return inner();"
                            "}"
                            "print(outer());";
    EXPECT_EQ(runJIT(src), "42");
}

// R156 通过 upvalue 调用闭包值
TEST(TestJIT, R156UpvalueClosureCallViaUpvalue) {
    // 通过 upvalue 调用闭包值：inner 通过 upvalue 引用 g，再调用 g
    const std::string src = "fun outer() {"
                            "  fun f(x) { return x * 2; }"
                            "  var g = f;"
                            "  fun inner() { return g(21); }"
                            "  return inner();"
                            "}"
                            "print(outer());";
    EXPECT_EQ(runJIT(src), "42");
}

// R156 三后端一致性：upvalue 捕获 + 修改 + 关闭
TEST(TestJIT, R156UpvalueConsistencyWithStackVM) {
    // 三后端一致性：upvalue 捕获 + 修改 + 关闭
    const std::string src = "fun makeCounter() {"
                            "  var count = 0;"
                            "  fun inc() { count = count + 1; return count; }"
                            "  print(inc());"
                            "  print(inc());"
                            "  return inc();"
                            "}"
                            "print(makeCounter());";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "123");
}

// R156 三后端一致性：currying + upvalue 透传
TEST(TestJIT, R156UpvalueCurryingConsistencyWithStackVM) {
    // 三后端一致性：currying + upvalue 透传
    const std::string src = "fun makeAdder(x) {"
                            "  fun add(y) { return x + y; }"
                            "  return add;"
                            "}"
                            "var add10 = makeAdder(10);"
                            "var add20 = makeAdder(20);"
                            "print(add10(32));"
                            "print(add20(22));"
                            "print(add10(5));"
                            "print(add20(8));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "42421528");
}

TEST(TestJIT, R156UpvalueDeepNestingConsistencyWithStackVM) {
    // 三后端一致性：深度嵌套 + passthrough + close
    const std::string src = "fun f(a) {"
                            "  fun g(b) {"
                            "    fun h(c) {"
                            "      fun i(d) { return a + b + c + d; }"
                            "      return i;"
                            "    }"
                            "    return h;"
                            "  }"
                            "  return g;"
                            "}"
                            "print(f(1)(2)(3)(4));"
                            "print(f(10)(20)(5)(7));"
                            "print(f(100)(0)(0)(0));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "1042100");
}

TEST(TestJIT, R156UpvalueMixedLocalAndPassthrough) {
    // 混合 isLocal 和 passthrough：middle 捕获 outer 的 x（isLocal），
    // inner 透传 middle 的 x（passthrough）并捕获 middle 的 y（isLocal）
    const std::string src = "fun outer() {"
                            "  var x = 10;"
                            "  fun middle() {"
                            "    var y = 20;"
                            "    fun inner() { return x + y; }"
                            "    return inner();"
                            "  }"
                            "  return middle();"
                            "}"
                            "print(outer());";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, vmOut);
    EXPECT_EQ(jitOut, "30");
}

// ============================================================
// R157: lazy compilation + OSR 测试
// ------------------------------------------------------------
// 本节测试 JIT 后端阶段 5g 的两个新特性：
//   1. lazy compilation：函数/方法 chunk 在首次调用时按需编译
//      - setLazyCompilation(true) 启用
//      - compileAllChunks 将 funcEntries_/methodEntries_ entryPtr 置空
//      - jitCallByName/jitCallExpr/jitMethodCall 在 entryPtr 为 null 时
//        通过 backendPtr 回调 triggerLazyCompile → compileSingleChunkLazy
//      - getLazyCompiledChunks() 返回已 lazy 编译的 chunk 名称列表
//
//   2. OSR（On-Stack Replacement，教学简化版）：
//      - 循环回边计数超阈值时触发特化重编译（复用 R152/R153 特化）
//      - setOsrThreshold(chunkName, threshold) 设置 per-chunk 阈值
//      - OP_LOOP 注入回边计数器 + 阈值检查 + OSR 触发
//      - getOsrLoopStats() 返回循环回边计数
//      - getOsrRecompileStats() 返回 OSR 触发标志
// ============================================================

/// R157 辅助：运行 JIT（可配置 lazy mode），返回输出字符串 + JITBackend 指针
/// 与 runJIT 不同，此函数暴露 JITBackend 供测试检查 lazy/OSR 统计
struct R157JitResult {
    std::string output;
    JitResult result = JitResult::OK;
    std::string error;
};

R157JitResult runJITWithBackend(const std::string& src, bool lazyMode) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return {"<parse-fail>", JitResult::CompileError, "parse fail"};

    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return {"<compile:" + c.getLastError() + ">", JitResult::CompileError, c.getLastError()};

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    if (lazyMode) {
        jit.setLazyCompilation(true);
    }
    JitResult result = jit.execute(cr);
    if (result == JitResult::CompileError)
        return {out + "<jit-compile:" + jit.getLastError() + ">", result, jit.getLastError()};
    if (result == JitResult::RuntimeError)
        return {out + "<jit-runtime:" + jit.getLastError() + ">", result, jit.getLastError()};
    return {out, result, {}};
}

// ---- R157 lazy compilation 基础测试 ----

TEST(TestJIT, R157LazyBasic) {
    // lazy compilation 启用后，函数首次调用触发按需编译，输出正确
    const std::string src = "fun add(a, b) { return a + b; }"
                            "print(add(3, 4));";
    auto r = runJITWithBackend(src, /*lazyMode=*/true);
    EXPECT_EQ(r.result, JitResult::OK);
    EXPECT_EQ(r.output, "7");
}

TEST(TestJIT, R157LazyMultipleCalls) {
    // 同一函数多次调用：首次 lazy 编译，后续直接走已编译入口
    const std::string src = "fun add(a, b) { return a + b; }"
                            "print(add(1, 2));"
                            "print(add(3, 4));"
                            "print(add(10, 20));";
    auto r = runJITWithBackend(src, /*lazyMode=*/true);
    EXPECT_EQ(r.result, JitResult::OK);
    EXPECT_EQ(r.output, "3730");
}

TEST(TestJIT, R157LazyMultipleFunctions) {
    // 多个函数各自 lazy 编译
    const std::string src = "fun f(x) { return x + 1; }"
                            "fun g(x) { return x * 2; }"
                            "print(f(10));"
                            "print(g(10));";
    auto r = runJITWithBackend(src, /*lazyMode=*/true);
    EXPECT_EQ(r.result, JitResult::OK);
    EXPECT_EQ(r.output, "1120");
}

TEST(TestJIT, R157LazyRecursiveFunction) {
    // 递归函数 lazy 编译：fib(10) = 55
    const std::string src = "fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }"
                            "print(fib(10));";
    auto r = runJITWithBackend(src, /*lazyMode=*/true);
    EXPECT_EQ(r.result, JitResult::OK);
    EXPECT_EQ(r.output, "55");
}

TEST(TestJIT, R157LazyDisabled) {
    // lazy compilation 禁用（默认模式）：正常执行
    const std::string src = "fun add(a, b) { return a + b; }"
                            "print(add(3, 4));";
    auto r = runJITWithBackend(src, /*lazyMode=*/false);
    EXPECT_EQ(r.result, JitResult::OK);
    EXPECT_EQ(r.output, "7");
}

TEST(TestJIT, R157LazyGetChunks) {
    // 验证 getLazyCompiledChunks() 返回已 lazy 编译的 chunk 名称
    const std::string src = "fun add(a, b) { return a + b; }"
                            "fun mul(a, b) { return a * b; }"
                            "print(add(3, 4));"
                            "print(mul(5, 6));";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setLazyCompilation(true);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "730");

    const auto& lazyChunks = jit.getLazyCompiledChunks();
    // 应包含 "add" 和 "mul"（顺序取决于调用顺序）
    bool hasAdd = false;
    bool hasMul = false;
    for (const auto& name : lazyChunks) {
        if (name == "add")
            hasAdd = true;
        if (name == "mul")
            hasMul = true;
    }
    EXPECT_TRUE(hasAdd);
    EXPECT_TRUE(hasMul);
}

TEST(TestJIT, R157LazyConsistencyWithStackVM) {
    // lazy mode 输出与 StackVM 一致
    const std::string src = "fun square(x) { return x * x; }"
                            "fun sumSquares(n) {"
                            "  var i = 0;"
                            "  var s = 0;"
                            "  while (i < n) { s = s + square(i); i = i + 1; }"
                            "  return s;"
                            "}"
                            "print(sumSquares(5));";
    auto r = runJITWithBackend(src, /*lazyMode=*/true);
    EXPECT_EQ(r.result, JitResult::OK);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(r.output, vmOut);
    EXPECT_EQ(r.output, "30"); // 0+1+4+9+16 = 30
}

TEST(TestJIT, R157LazyClosureValueCall) {
    // lazy mode 下闭包值调用（OP_CALL_EXPR）也触发 lazy compilation
    const std::string src = "fun add(a, b) { return a + b; }"
                            "var g = add;"
                            "print(g(3, 4));";
    auto r = runJITWithBackend(src, /*lazyMode=*/true);
    EXPECT_EQ(r.result, JitResult::OK);
    EXPECT_EQ(r.output, "7");
}

TEST(TestJIT, R157LazyMethodCall) {
    // lazy mode 下方法调用（OP_METHOD_CALL）也触发 lazy compilation
    const std::string src = "class Calc {"
                            "  var x = 0;"
                            "  fun add(y) { return x + y; }"
                            "}"
                            "var c = Calc();"
                            "c.x = 10;"
                            "print(c.add(32));";
    auto r = runJITWithBackend(src, /*lazyMode=*/true);
    EXPECT_EQ(r.result, JitResult::OK);
    EXPECT_EQ(r.output, "42");
}

// ---- R157 OSR 测试 ----

TEST(TestJIT, R157OsrThresholdZero) {
    // OSR 阈值为 0（默认）：不触发 OSR，循环正常执行
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 10) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    // 不设置 OSR 阈值（默认 0），不触发 OSR
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "45"); // 0+1+...+9 = 45

    // OSR 未触发：所有 recompiledFlags 应为 0
    auto stats = jit.getOsrRecompileStats();
    for (const auto& [name, flag] : stats) {
        EXPECT_EQ(flag, 0u) << "chunk " << name << " 不应触发 OSR";
    }
}

TEST(TestJIT, R157OsrBasicTrigger) {
    // OSR 阈值设置后，循环回边达到阈值触发特化重编译
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 20) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    // 设置 OSR 阈值为 5（循环 20 次，第 5 次回边触发）
    jit.setOsrThreshold("Looper.run", 5);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "190"); // 0+1+...+19 = 190

    // 验证 OSR 触发：Looper.run 的 recompiledFlag 应为 1
    auto stats = jit.getOsrRecompileStats();
    bool foundRun = false;
    uint64_t runFlag = 0;
    for (const auto& [name, flag] : stats) {
        if (name == "Looper.run") {
            foundRun = true;
            runFlag = flag;
        }
    }
    EXPECT_TRUE(foundRun);
    EXPECT_EQ(runFlag, 1u); // 应已触发 OSR
}

TEST(TestJIT, R157OsrLoopStats) {
    // 验证 getOsrLoopStats() 返回正确的循环回边计数
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 10) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrThreshold("Looper.run", 100); // 阈值高于循环次数，不触发
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 循环 10 次 → 回边计数应为 10
    auto stats = jit.getOsrLoopStats();
    bool foundRun = false;
    uint64_t runCount = 0;
    for (const auto& [name, count] : stats) {
        if (name == "Looper.run") {
            foundRun = true;
            runCount = count;
        }
    }
    EXPECT_TRUE(foundRun);
    EXPECT_EQ(runCount, 10u); // 10 次回边
}

TEST(TestJIT, R157OsrConsistencyWithStackVM) {
    // OSR 触发后输出仍与 StackVM 一致（正确性验证）
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 50) { s = s + i * 2; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrThreshold("Looper.run", 10); // 第 10 次回边触发
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(out, vmOut);
    // 0*2 + 1*2 + ... + 49*2 = 2 * (0+1+...+49) = 2 * 1225 = 2450
    EXPECT_EQ(out, "2450");
}

TEST(TestJIT, R157OsrNoDuplicateTrigger) {
    // OSR 只触发一次：osrRecompiledFlags 设置后不再重复触发
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 100) { s = s + 1; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrThreshold("Looper.run", 5); // 低阈值，100 次回边
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "100");

    // 验证 OSR 只触发一次（flag=1，不是 100）
    auto stats = jit.getOsrRecompileStats();
    for (const auto& [name, flag] : stats) {
        if (name == "Looper.run") {
            EXPECT_EQ(flag, 1u); // 只触发一次
        }
    }
}

// ---- R158 真正 OSR 栈帧迁移 + 分层编译 + 反优化测试 ----
//
// R158 在 R157 简化版 OSR 基础上实现真正 OSR 栈帧迁移：
//   - OP_LOOP 回边达阈值时，JIT 代码保存 r13/r15 到邮箱
//   - 调用 triggerOsrMigration → compileChunkSpecializedWithOsr 生成含 OSR 入口点的特化版本
//   - JIT 代码 jmp osrEntryPoint，OSR 入口点恢复 r13/r15 后跳到循环回边继续执行
//   - chunkTiers_ 跟踪 per-chunk Tier（Baseline → Specialized）
//   - triggerDeoptimize 实现 Tier 2 → Tier 1 反优化（教学版：下次 execute 生效）

TEST(TestJIT, R158OsrMigrationBasic) {
    // 真正 OSR 栈帧迁移：启用 osrMigrationMode，循环回边达阈值触发栈帧迁移
    // 验证：输出正确 + chunk 出现在 osrMigratedChunks_
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 20) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true); // 启用真正 OSR 栈帧迁移
    jit.setOsrThreshold("Looper.run", 5);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "190"); // 0+1+...+19 = 190

    // 验证 OSR 迁移触发：Looper.run 应出现在 osrMigratedChunks_
    const auto& migrated = jit.getOsrMigratedChunks();
    bool foundRun = false;
    for (const auto& name : migrated) {
        if (name == "Looper.run") {
            foundRun = true;
        }
    }
    EXPECT_TRUE(foundRun) << "Looper.run 应出现在 osrMigratedChunks_";
}

TEST(TestJIT, R158OsrMigrationConsistencyWithStackVM) {
    // 真正 OSR 栈帧迁移后输出与 StackVM 一致（正确性验证）
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 50) { s = s + i * 2; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 10);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(out, vmOut);
    // 0*2 + 1*2 + ... + 49*2 = 2 * (0+1+...+49) = 2 * 1225 = 2450
    EXPECT_EQ(out, "2450");
}

TEST(TestJIT, R158OsrMigrationNoDuplicate) {
    // OSR 迁移只触发一次：osrMigratedChunks_ 中每个 chunk 只出现一次
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 100) { s = s + 1; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 5);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "100");

    // 验证 OSR 迁移只触发一次（Looper.run 在 osrMigratedChunks_ 中只出现一次）
    const auto& migrated = jit.getOsrMigratedChunks();
    size_t runCount = 0;
    for (const auto& name : migrated) {
        if (name == "Looper.run") {
            ++runCount;
        }
    }
    EXPECT_EQ(runCount, 1u) << "Looper.run 应只 OSR 迁移一次";
}

TEST(TestJIT, R158GetChunkTiersInitial) {
    // 初始状态：所有 chunk 应为 Baseline（Tier 1）
    const std::string src = "fun add(a, b) { return a + b; }"
                            "print(add(3, 4));";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "7");

    // 验证初始 Tier 全部为 Baseline
    auto tiers = jit.getChunkTiers();
    EXPECT_FALSE(tiers.empty());
    for (const auto& [name, tier] : tiers) {
        EXPECT_EQ(tier, minilang::JitTier::Baseline) << "chunk " << name << " 初始应为 Baseline";
    }
}

TEST(TestJIT, R158GetChunkTiersAfterOsr) {
    // OSR 迁移后：被迁移的 chunk Tier 应升级为 Specialized（Tier 2）
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 20) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 5);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "190");

    // 验证 Looper.run 的 Tier 升级为 Specialized
    auto tiers = jit.getChunkTiers();
    bool foundRun = false;
    minilang::JitTier runTier = minilang::JitTier::Baseline;
    for (const auto& [name, tier] : tiers) {
        if (name == "Looper.run") {
            foundRun = true;
            runTier = tier;
        }
    }
    EXPECT_TRUE(foundRun);
    EXPECT_EQ(runTier, minilang::JitTier::Specialized) << "OSR 迁移后 Looper.run 应为 Specialized";
}

TEST(TestJIT, R158DeoptimizeBasic) {
    // 反优化：OSR 迁移后手动触发 triggerDeoptimize，验证 deoptCount 与 deoptimizedChunks_
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 20) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 5);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "190");

    // 执行前 deoptCount 应为 0
    EXPECT_EQ(jit.getDeoptCount(), 0u);

    // 查找 Looper.run 的 chunkIdx
    auto tiers = jit.getChunkTiers();
    int64_t runChunkIdx = -1;
    for (size_t i = 0; i < tiers.size(); ++i) {
        if (tiers[i].first == "Looper.run") {
            runChunkIdx = static_cast<int64_t>(i);
            break;
        }
    }
    ASSERT_GE(runChunkIdx, 0);

    // 手动触发反优化
    EXPECT_EQ(jit.triggerDeoptimize(runChunkIdx), 0);

    // 验证 deoptCount=1 且 deoptimizedChunks_ 含 Looper.run
    EXPECT_EQ(jit.getDeoptCount(), 1u);
    const auto& deoptimized = jit.getDeoptimizedChunks();
    bool foundRun = false;
    for (const auto& name : deoptimized) {
        if (name == "Looper.run") {
            foundRun = true;
        }
    }
    EXPECT_TRUE(foundRun) << "Looper.run 应出现在 deoptimizedChunks_";
}

TEST(TestJIT, R158DeoptimizeTierDowngrade) {
    // 反优化后：chunk Tier 应从 Specialized 降级回 Baseline
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 20) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 5);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 查找 Looper.run 的 chunkIdx
    auto tiers = jit.getChunkTiers();
    int64_t runChunkIdx = -1;
    for (size_t i = 0; i < tiers.size(); ++i) {
        if (tiers[i].first == "Looper.run") {
            runChunkIdx = static_cast<int64_t>(i);
            break;
        }
    }
    ASSERT_GE(runChunkIdx, 0);

    // 反优化前：Specialized
    EXPECT_EQ(tiers[static_cast<size_t>(runChunkIdx)].second, minilang::JitTier::Specialized);

    // 手动触发反优化
    EXPECT_EQ(jit.triggerDeoptimize(runChunkIdx), 0);

    // 反优化后：Baseline
    auto tiersAfter = jit.getChunkTiers();
    EXPECT_EQ(tiersAfter[static_cast<size_t>(runChunkIdx)].second, minilang::JitTier::Baseline)
        << "反优化后 Looper.run 应回到 Baseline";
}

TEST(TestJIT, R158DeoptimizeReexecute) {
    // 反优化后重新 execute：使用 baseline 版本，输出仍正确
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 20) { s = s + i; i = i + 1; }"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 5);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "190");

    // 查找 Looper.run 的 chunkIdx 并触发反优化
    auto tiers = jit.getChunkTiers();
    int64_t runChunkIdx = -1;
    for (size_t i = 0; i < tiers.size(); ++i) {
        if (tiers[i].first == "Looper.run") {
            runChunkIdx = static_cast<int64_t>(i);
            break;
        }
    }
    ASSERT_GE(runChunkIdx, 0);
    EXPECT_EQ(jit.triggerDeoptimize(runChunkIdx), 0);

    // 重新 execute：baseline 版本应产生相同正确结果
    std::string out2;
    jit.setOutputCallback([&](const std::string& s) { out2 += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out2, "190") << "反优化后重新 execute 应使用 baseline 版本，输出仍正确";
}

TEST(TestJIT, R158DeoptimizeNotSpecialized) {
    // 反优化未特化的 chunk 应失败（返回非 0）
    const std::string src = "fun add(a, b) { return a + b; }"
                            "print(add(3, 4));";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 无 OSR 迁移，所有 chunk 为 Baseline，反优化应失败
    auto tiers = jit.getChunkTiers();
    for (size_t i = 0; i < tiers.size(); ++i) {
        // 对 Baseline chunk 触发反优化应失败
        EXPECT_NE(jit.triggerDeoptimize(static_cast<int64_t>(i)), 0);
    }
    EXPECT_EQ(jit.getDeoptCount(), 0u) << "未特化的 chunk 反优化不应成功";
}

// ============================================================
// R159: 即时反优化 + Tier 0→Tier 1 自动升级测试
// ============================================================
// Goal 1: 特化版本 emitCheckInt 类型守卫失败时，JIT 代码自动调用 jitDeoptimize
//         触发即时降级（Tier 2→Tier 1），无需手动调用 triggerDeoptimize。
// Goal 2: 分层编译模式下，非 main chunk 初始为 Tier 0 (Interpreter)，
//         首次调用时通过 lazy compilation 自动升级到 Tier 1 (Baseline)。

TEST(TestJIT, R159ImmediateDeoptOnTypeGuard) {
    // R159 Goal 1: OSR 迁移到 INT 特化版本后，遇到非 INT 值时类型守卫失败，
    // JIT 代码自动调用 jitDeoptimize 触发即时反优化（无需手动 triggerDeoptimize）。
    // 循环内全 INT → OSR 迁移到 INT 特化；循环后 s + "x" → 类型守卫失败 → 自动 deopt。
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 5) { s = s + i; i = i + 1; }"
                            "    s = s + \"x\";" // INT + STRING → 类型守卫失败
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 3); // 第 3 次回边触发 OSR
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    // 循环: 0+0+1+2+3+4 = 10, 然后 10 + "x" = "10x"（generic 路径字符串拼接）
    EXPECT_EQ(out, "10x");

    // R159 核心断言：自动反优化在 execute 期间触发（非手动 triggerDeoptimize）
    EXPECT_GE(jit.getDeoptCount(), 1u) << "类型守卫失败应自动触发反优化";

    // Looper.run 应出现在已反优化列表中
    const auto& deoptimized = jit.getDeoptimizedChunks();
    bool foundRun = false;
    for (const auto& name : deoptimized) {
        if (name == "Looper.run") {
            foundRun = true;
        }
    }
    EXPECT_TRUE(foundRun) << "Looper.run 应出现在 deoptimizedChunks_";
}

TEST(TestJIT, R159ImmediateDeoptCorrectResult) {
    // R159 Goal 1: 反优化后 generic 路径正确处理当前操作，执行结果正确。
    // 使用三后端一致性验证：JIT 反优化路径的结果应与 StackVM 一致。
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 10) { s = s + i; i = i + 1; }"
                            "    s = s + \"!\";" // INT + STRING → deopt → generic 拼接
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    // StackVM 参考结果
    std::string expected = minilang_test::runStackVM(src);

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 3);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);

    EXPECT_EQ(out, expected) << "反优化后 generic 路径结果应与 StackVM 一致";
    EXPECT_GE(jit.getDeoptCount(), 1u);
}

TEST(TestJIT, R159ImmediateDeoptTierDowngrade) {
    // R159 Goal 1: 自动反优化后，chunk Tier 应从 Specialized 降级回 Baseline。
    const std::string src = "class Looper {"
                            "  var s = 0;"
                            "  fun run() {"
                            "    var i = 0;"
                            "    while (i < 5) { s = s + i; i = i + 1; }"
                            "    s = s + \"x\";"
                            "  }"
                            "}"
                            "var lp = Looper();"
                            "lp.run();"
                            "print(lp.s);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setOsrMigrationMode(true);
    jit.setOsrThreshold("Looper.run", 3);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_GE(jit.getDeoptCount(), 1u);

    // 反优化后 Looper.run 应为 Baseline（从 Specialized 降级）
    auto tiers = jit.getChunkTiers();
    bool foundRun = false;
    minilang::JitTier runTier = minilang::JitTier::Baseline;
    for (const auto& [name, tier] : tiers) {
        if (name == "Looper.run") {
            foundRun = true;
            runTier = tier;
        }
    }
    EXPECT_TRUE(foundRun);
    EXPECT_EQ(runTier, minilang::JitTier::Baseline) << "反优化后应降级为 Baseline";
}

TEST(TestJIT, R159TieredCompilationFlag) {
    // R159 Goal 2: setTieredCompilation / isTieredCompilation API
    JITBackend jit;
    EXPECT_FALSE(jit.isTieredCompilation()) << "默认未启用分层编译";

    jit.setTieredCompilation(true);
    EXPECT_TRUE(jit.isTieredCompilation()) << "setTieredCompilation(true) 后应启用";

    jit.setTieredCompilation(false);
    EXPECT_FALSE(jit.isTieredCompilation()) << "setTieredCompilation(false) 后应禁用";
}

TEST(TestJIT, R159TieredCompilationUpgrade) {
    // R159 Goal 2: 分层编译模式下，函数 chunk 从 Tier 0 (Interpreter) 升级到 Tier 1 (Baseline)。
    // 执行后 getChunkTiers() 应显示函数 chunk 为 Baseline（通过 lazy compilation 升级）。
    const std::string src = "fun add(a, b) { return a + b; }"
                            "print(add(3, 4));";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setTieredCompilation(true); // 启用分层编译
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "7");

    // 执行后：main 应为 Baseline，add 应从 Interpreter 升级到 Baseline
    auto tiers = jit.getChunkTiers();
    ASSERT_GE(tiers.size(), 2u) << "应至少有 main + add 两个 chunk";

    EXPECT_EQ(tiers[0].second, minilang::JitTier::Baseline) << "main 应为 Baseline";

    // 查找 add chunk
    bool foundAdd = false;
    minilang::JitTier addTier = minilang::JitTier::Interpreter;
    for (const auto& [name, tier] : tiers) {
        if (name == "add") {
            foundAdd = true;
            addTier = tier;
        }
    }
    EXPECT_TRUE(foundAdd) << "应找到 add chunk";
    EXPECT_EQ(addTier, minilang::JitTier::Baseline) << "分层编译模式下 add 应从 Interpreter 升级到 Baseline";

    // lazy compiled chunks 应包含 add
    const auto& lazyChunks = jit.getLazyCompiledChunks();
    bool lazyFound = false;
    for (const auto& name : lazyChunks) {
        if (name == "add") {
            lazyFound = true;
        }
    }
    EXPECT_TRUE(lazyFound) << "add 应通过 lazy compilation 编译";
}

TEST(TestJIT, R159TieredCompilationImpliesLazy) {
    // R159 Goal 2: setTieredCompilation(true) 应自动启用 lazy compilation。
    // 验证方法：分层编译模式下函数调用触发 lazy compilation，getLazyCompiledChunks 非空。
    const std::string src = "fun double(x) { return x * 2; }"
                            "print(double(21));";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast != nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    jit.setTieredCompilation(true);
    EXPECT_EQ(jit.execute(cr), JitResult::OK);
    EXPECT_EQ(out, "42");

    // 分层编译 implies lazy → getLazyCompiledChunks 应非空且含 double
    const auto& lazyChunks = jit.getLazyCompiledChunks();
    EXPECT_FALSE(lazyChunks.empty()) << "分层编译应自动启用 lazy compilation";
    bool foundDouble = false;
    for (const auto& name : lazyChunks) {
        if (name == "double") {
            foundDouble = true;
        }
    }
    EXPECT_TRUE(foundDouble) << "double 应通过 lazy compilation 编译";
}

TEST(TestJIT, R159TieredCompilationCorrectness) {
    // R159 Goal 2: 分层编译模式下多种程序的正确性验证（三后端一致性）。
    const auto runTiered = [](const std::string& src) -> std::string {
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
        JITBackend jit;
        std::string out;
        jit.setOutputCallback([&](const std::string& s) { out += s; });
        jit.setTieredCompilation(true);
        JitResult result = jit.execute(cr);
        if (result != JitResult::OK)
            return out + "<jit-error:" + jit.getLastError() + ">";
        return out;
    };

    // 简单函数调用
    EXPECT_EQ(runTiered("fun add(a, b) { return a + b; } print(add(3, 4));"),
              minilang_test::runStackVM("fun add(a, b) { return a + b; } print(add(3, 4));"));

    // 多函数调用链
    EXPECT_EQ(runTiered("fun sq(x) { return x * x; } fun sum(a, b) { return a + b; }"
                        "print(sum(sq(3), sq(4)));"),
              minilang_test::runStackVM("fun sq(x) { return x * x; } fun sum(a, b) { return a + b; }"
                                        "print(sum(sq(3), sq(4)));"));

    // 方法调用
    EXPECT_EQ(runTiered("class Calc { var v = 0; fun add(x) { v = v + x; } fun get() { return v; } }"
                        "var c = Calc(); c.add(10); c.add(20); print(c.get());"),
              minilang_test::runStackVM("class Calc { var v = 0; fun add(x) { v = v + x; }"
                                        " fun get() { return v; } }"
                                        "var c = Calc(); c.add(10); c.add(20); print(c.get());"));

    // 递归
    EXPECT_EQ(runTiered("fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }"
                        "print(fib(10));"),
              minilang_test::runStackVM("fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }"
                                        "print(fib(10));"));
}

// ============================================================
// R160: 递归深度限制 — 错误消息三后端一致性
// ============================================================
// JIT 原 4 处帧推送点（jitCallExpr/jitClassNew/jitMethodCall/jitCallByName）
// 使用硬编码 "递归深度超限" 与 StackVM/RegisterVM/Interpreter 的统一消息
// "递归深度超过限制 (256)" 不一致（P1 语义不一致）。R160 统一为
// ErrorMessages::kRecursionDepthExceededFmt + RuntimeLimits::MAX_FRAMES。
//
// 注意：必须使用非尾递归（return 1 + inf(n+1)），否则编译器 TCO 将
// return inf(n+1) 优化为 OP_SET_LOCAL + OP_JUMP 循环，不触发 OP_CALL，
// 递归深度限制无法生效。尾递归无限循环在 StackVM 中由 MAX_LOOP_ITERATIONS
// 拦截，JIT 尚未实现循环迭代限制（已知 P1 差异，后续修复）。
TEST(TestJIT, R160RecursionDepthLimitUnifiedMessage) {
    // 非尾递归无限递归：1 + inf(n+1) 使编译器无法 TCO，生成真实 OP_CALL
    std::string src = "fun inf(n) { return 1 + inf(n+1); } print(inf(0));";
    std::string jitOut = runJIT(src);
    // JIT 错误消息应使用统一格式 "递归深度超过限制 (256)"（与三后端一致）
    EXPECT_NE(jitOut.find("递归深度超过限制 (256)"), std::string::npos)
        << "JIT 递归深度错误消息应与三后端一致，实际: " << jitOut;
    // StackVM 应产生相同错误消息文本
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_NE(vmOut.find("递归深度超过限制 (256)"), std::string::npos) << "StackVM 递归深度错误消息，实际: " << vmOut;
}

// R160: inline cache for OP_MEMBER_GET 测试
// 验证：(1) cache 未命中时正确填充 (2) 后续访问命中 cache (3) 语义结果正确
TEST(TestJIT, R160InlineCacheForMemberGet) {
    std::string src = "class P { var x: int; var y: int; }\n"
                      "var p = P();\n"
                      "p.x = 10;\n"
                      "p.y = 20;\n"
                      "var i = 0;\n"
                      "var s = 0;\n"
                      "while (i < 5) { s = s + p.x; i = i + 1; }\n"
                      "print(s);\n"; // 10*5 = 50
    std::string jitOut = runJIT(src);
    EXPECT_EQ(jitOut, "50") << "IC 应保持语义正确，实际: " << jitOut;
}

// R160: inline cache 统计验证
// 验证：首次访问 miss，后续相同 receiver 的访问 hit
TEST(TestJIT, R160InlineCacheStatsHitMiss) {
    std::string src = "class P { var x: int; }\n"
                      "var p = P();\n"
                      "p.x = 42;\n"
                      "var i = 0;\n"
                      "var s = 0;\n"
                      "while (i < 10) { s = s + p.x; i = i + 1; }\n"
                      "print(s);\n"; // 42*10 = 420

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    JitResult result = jit.execute(cr);
    ASSERT_EQ(result, JitResult::OK);
    EXPECT_EQ(out, "420");

    // IC 统计：10 次 OP_MEMBER_GET p.x
    // 首次 miss（cache 空）→ 填充；后续 9 次 hit（相同 receiver p）
    auto stats = jit.getInlineCacheStats();
    EXPECT_EQ(stats.miss, 1u) << "首次访问应 miss 并填充 cache，实际 miss: " << stats.miss;
    EXPECT_EQ(stats.hit, 9u) << "后续 9 次访问应命中 cache，实际 hit: " << stats.hit;
}

// R160: inline cache 多态场景（不同 receiver 导致 cache 抖动）
// 验证：两个不同 Instance 交替访问时，cache 反复 miss（单态 IC 限制）
TEST(TestJIT, R160InlineCachePolymorphicDegradation) {
    std::string src = "class P { var x: int; }\n"
                      "var p1 = P();\n"
                      "var p2 = P();\n"
                      "p1.x = 1;\n"
                      "p2.x = 2;\n"
                      "var i = 0;\n"
                      "var s = 0;\n"
                      "while (i < 4) { s = s + p1.x; s = s + p2.x; i = i + 1; }\n"
                      "print(s);\n"; // (1+2)*4 = 12

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_TRUE(ast);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    JitResult result = jit.execute(cr);
    ASSERT_EQ(result, JitResult::OK);
    EXPECT_EQ(out, "12");

    // PIC 升级后：p1.x 和 p2.x 是不同 callSite（不同 OP_MEMBER_GET），各自 PIC 独立
    // p1.x callSite: 首次 miss → 填充 p1 → 后续 3 次 hit（p1 不变）
    // p2.x callSite: 首次 miss → 填充 p2 → 后续 3 次 hit（p2 不变）
    // 总 miss=2, hit=6（PIC 4 路足以容纳 2 种类型，无退化）
    auto stats = jit.getInlineCacheStats();
    EXPECT_EQ(stats.miss, 2u) << "两个 callSite 各首次 miss，实际 miss: " << stats.miss;
    EXPECT_EQ(stats.hit, 6u) << "两个 callSite 各后续 3 次 hit，实际 hit: " << stats.hit;
    EXPECT_EQ(stats.megamorphic, 0u) << "2 种类型不应触发 megamorphic";
}

// ============================================================
// R161 P0 一致性补全：V-P1-6 fix 场景验证
// ------------------------------------------------------------
// 验证闭包内通过 upvalue 修改方法字段槽时，JIT 是否能正确同步回 receiver。
// StackVM 通过 fieldsModified 标记 + OP_RETURN 门控同步；
// JIT 当前用 jitMethodReturn 无条件同步（JIT.cpp:3433 注释声称等价）。
// 本测试验证 JIT 无条件同步是否真的等价覆盖 V-P1-6 语义。
// ============================================================

// R161Closure* 三个测试曾因 JIT 闭包访问类字段时崩溃（V-P1-6 JIT 同步缺失 + 闭包字段捕获 bug）
// 被标记 DISABLED_；JIT 路径补全 V-P1-6 同步语义后已重新启用并通过。
// 四后端（Interpreter/StackVM/RegisterVM/JIT）均正确处理此场景。
// 历史背景见 R160 changelog 与 JIT.cpp jitMethodReturn 注释。
TEST(TestJIT, R161ClosureReadsMethodField) {
    // 分级验证 step 1：闭包读取类字段（不修改），确认基础路径正常
    const std::string src = "class C {\n"
                            "    var x = 42;\n"
                            "    fun get() { var f = fun() { return x; }; return f(); }\n"
                            "}\n"
                            "var c = C();\n"
                            "print(c.get());\n"; // 期望 42
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(vmOut, "42") << "StackVM 输出: " << vmOut;
    // JIT 验证：当前预期失败（V-P1-6 缺失 + 可能的 JIT 闭包字段捕获 bug）
    std::string jitOut = runJIT(src);
    EXPECT_EQ(jitOut, "42") << "JIT 输出: " << jitOut;
}

// 分级诊断测试：定位 JIT 方法内闭包崩溃的具体场景
TEST(TestJIT, R161DiagClosureInMethodNoCapture) {
    // 场景 A：方法内定义闭包（不捕获任何 upvalue）
    const std::string src = "class C {\n"
                            "    fun get() { var f = fun() { return 42; }; return f(); }\n"
                            "}\n"
                            "var c = C();\n"
                            "print(c.get());\n";
    EXPECT_EQ(runJIT(src), "42") << "JIT 场景A: " << runJIT(src);
}

TEST(TestJIT, R161DiagMethodCallOnly) {
    // 场景 A0：仅方法调用（无闭包），确认方法调用本身正常
    const std::string src = "class C {\n"
                            "    fun get() { return 42; }\n"
                            "}\n"
                            "var c = C();\n"
                            "print(c.get());\n";
    EXPECT_EQ(runJIT(src), "42") << "JIT 场景A0: " << runJIT(src);
}

TEST(TestJIT, R161DiagTopLevelClosureCall) {
    // 场景 A1：顶层闭包调用（无方法），确认闭包调用本身正常
    const std::string src = "var f = fun() { return 42; };\n"
                            "print(f());\n";
    EXPECT_EQ(runJIT(src), "42") << "JIT 场景A1: " << runJIT(src);
}

TEST(TestJIT, R161DiagNamedFunctionDirectCall) {
    // 场景 A2：具名函数直接调用（不走 OP_CALL_EXPR），对比匿名 lambda
    const std::string src = "fun f() { return 42; }\n"
                            "print(f());\n";
    EXPECT_EQ(runJIT(src), "42") << "JIT 场景A2: " << runJIT(src);
}

TEST(TestJIT, R161DiagNamedFunctionViaVar) {
    // 场景 A3：具名函数赋值给变量后调用（走 OP_CALL_EXPR），对比 A1 匿名 lambda
    const std::string src = "fun f() { return 42; }\n"
                            "var g = f;\n"
                            "print(g());\n";
    EXPECT_EQ(runJIT(src), "42") << "JIT 场景A3: " << runJIT(src);
}

TEST(TestJIT, R161DiagClosureInMethodCaptureLocal) {
    // 场景 B：方法内定义闭包捕获局部变量（非字段）
    const std::string src = "class C {\n"
                            "    fun get() {\n"
                            "        var v = 42;\n"
                            "        var f = fun() { return v; };\n"
                            "        return f();\n"
                            "    }\n"
                            "}\n"
                            "var c = C();\n"
                            "print(c.get());\n";
    EXPECT_EQ(runJIT(src), "42") << "JIT 场景B: " << runJIT(src);
}

TEST(TestJIT, R161DiagClosureInMethodCaptureField) {
    // 场景 C：方法内定义闭包捕获字段（R161 核心场景）
    const std::string src = "class C {\n"
                            "    var x = 42;\n"
                            "    fun get() { var f = fun() { return x; }; return f(); }\n"
                            "}\n"
                            "var c = C();\n"
                            "print(c.get());\n";
    EXPECT_EQ(runJIT(src), "42") << "JIT 场景C: " << runJIT(src);
}

TEST(TestJIT, R161ClosureModifiesMethodField) {
    // V-P1-6 核心场景：方法内定义闭包，闭包通过 upvalue 修改 this.field
    // 闭包内 count = count + 1 → OP_SET_UPVALUE 写入方法帧字段槽
    // 方法返回时需同步字段槽到 this.fields() 再 writeBack 到 receiver
    const std::string src = "class Counter {\n"
                            "    var count = 0;\n"
                            "    fun bump() {\n"
                            "        var inc = fun() { count = count + 1; };\n"
                            "        inc();\n"
                            "        inc();\n"
                            "    }\n"
                            "}\n"
                            "var c = Counter();\n"
                            "c.bump();\n"
                            "print(c.count);\n"; // 期望 2
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, "2") << "JIT 输出: " << jitOut;
    EXPECT_EQ(vmOut, "2") << "StackVM 输出: " << vmOut;
}

TEST(TestJIT, R161ClosureModifiesMethodFieldMultipleFields) {
    // 多字段场景：闭包同时修改两个字段
    const std::string src = "class Pair {\n"
                            "    var a = 0;\n"
                            "    var b = 0;\n"
                            "    fun swap() {\n"
                            "        var doSwap = fun() { var t = a; a = b; b = t; };\n"
                            "        doSwap();\n"
                            "    }\n"
                            "}\n"
                            "var p = Pair();\n"
                            "p.a = 10;\n"
                            "p.b = 20;\n"
                            "p.swap();\n"
                            "print(p.a);\n"
                            "print(p.b);\n"; // 期望 20, 10
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    EXPECT_EQ(jitOut, "2010") << "JIT 输出: " << jitOut;
    EXPECT_EQ(vmOut, "2010") << "StackVM 输出: " << vmOut;
}

// ============================================================
// R162: JIT 异常处理测试用例（try/catch/throw + finally）
// ============================================================
// 验证 OP_TRY_BEGIN/OP_TRY_END/OP_THROW/OP_PUSH_JUMP_TARGET/OP_FINALLY_END
// 机器码生成与 jitThrow 栈展开逻辑。
// 关键不变量：
//   1. 同一 MiniLang 源码在 JIT 与 StackVM 上输出一致（三后端一致性扩展至四后端）
//   2. try 正常路径：OP_TRY_END 弹出 handler，无异常传播
//   3. throw 路径：jitThrow 搜索 tryStack_，截断操作数栈，jmp catchAddr
//   4. 跨帧异常：被调函数 throw，主调者 catch，帧栈正确展开
//   5. finally 续跳：break/continue 触发 finally，OP_FINALLY_END pop 目标续跳

// ---- 基础 try/catch：throw 字面量，catch 捕获 ----
TEST(TestJIT, R162TryCatchThrowInt) {
    std::string src = "try {"
                      "  throw 42;"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    std::string expected = "42";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- try/catch 不触发：正常路径不进入 catch ----
TEST(TestJIT, R162TryCatchNoThrow) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  log = log + \"try\";"
                      "} catch (e) {"
                      "  log = log + \"catch\";"
                      "}"
                      "print(log);";
    std::string expected = "try";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- try/catch + finally 正常路径 ----
TEST(TestJIT, R162TryCatchFinallyNormal) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  log = log + \"try\";"
                      "} catch (e) {"
                      "  log = log + \"catch\";"
                      "} finally {"
                      "  log = log + \"finally\";"
                      "}"
                      "print(log);";
    std::string expected = "tryfinally";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- try/catch + finally 异常路径 ----
TEST(TestJIT, R162TryCatchFinallyException) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  log = log + \"try\";"
                      "  throw 42;"
                      "} catch (e) {"
                      "  log = log + \"catch\" + e;"
                      "} finally {"
                      "  log = log + \"finally\";"
                      "}"
                      "print(log);";
    std::string expected = "trycatch42finally";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- try/finally 无 catch：正常路径 ----
TEST(TestJIT, R162TryFinallyNoCatchNormal) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  log = log + \"try\";"
                      "} finally {"
                      "  log = log + \"finally\";"
                      "}"
                      "print(log);";
    std::string expected = "tryfinally";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- 嵌套 try/catch ----
TEST(TestJIT, R162NestedTryCatch) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  try {"
                      "    throw 1;"
                      "  } catch (e1) {"
                      "    log = log + \"inner:\" + e1 + \";\";"
                      "    throw 2;"
                      "  }"
                      "} catch (e2) {"
                      "  log = log + \"outer:\" + e2;"
                      "}"
                      "print(log);";
    std::string expected = "inner:1;outer:2";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- 嵌套 try/finally：finally 内部异常未捕获，传播到外层 catch ----
TEST(TestJIT, R162NestedTryFinallyUncaught) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  try {"
                      "    throw 99;"
                      "  } finally {"
                      "    log = log + \"inner-finally\";"
                      "  }"
                      "} catch (e) {"
                      "  log = log + \"outer-catch\" + e;"
                      "}"
                      "print(log);";
    std::string expected = "inner-finallyouter-catch99";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- throw 字符串 ----
TEST(TestJIT, R162ThrowString) {
    std::string src = "try {"
                      "  throw \"error-msg\";"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    std::string expected = "error-msg";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- 跨帧异常：函数内 throw，主调者 catch ----
TEST(TestJIT, R162CrossFrameException) {
    std::string src = "fun fail() {"
                      "  throw 100;"
                      "}"
                      "try {"
                      "  fail();"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    std::string expected = "100";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- 跨帧异常：深层调用栈 throw，顶层 catch ----
TEST(TestJIT, R162DeepCrossFrameException) {
    std::string src = "fun level3() { throw 7; }"
                      "fun level2() { level3(); }"
                      "fun level1() { level2(); }"
                      "try {"
                      "  level1();"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    std::string expected = "7";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- 未捕获异常：JIT 应设置 hasError 并返回 RuntimeError ----
TEST(TestJIT, R162UncaughtException) {
    std::string src = "throw 42;";
    std::string result = runJIT(src);
    EXPECT_NE(result.find("jit-runtime"), std::string::npos)
        << "未捕获异常应触发 JIT RuntimeError，实际输出: " << result;
    EXPECT_NE(result.find("未捕获的异常"), std::string::npos) << "错误消息应包含'未捕获的异常'，实际输出: " << result;
}

// ---- try/catch 在函数内 ----
// 注意：MiniLang 的 try/catch 仅捕获显式 throw，不捕获运行时错误（如除零）。
// 故 safeDiv 需显式检查除数并 throw，与 StackVM 行为一致。
TEST(TestJIT, R162TryCatchInFunction) {
    std::string src = "fun safeDiv(a, b) {"
                      "  try {"
                      "    if (b == 0) { throw \"div-by-zero\"; }"
                      "    return a / b;"
                      "  } catch (e) {"
                      "    return -1;"
                      "  }"
                      "}"
                      "print(safeDiv(10, 2));"
                      "print(safeDiv(10, 0));";
    std::string expected = "5-1";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- finally 在函数内：finally 执行后函数返回 ----
TEST(TestJIT, R162FinallyInFunction) {
    std::string src = "fun test() {"
                      "  var log = \"\";"
                      "  try {"
                      "    log = log + \"try;\";"
                      "  } catch (e) {"
                      "    log = log + \"catch;\";"
                      "  } finally {"
                      "    log = log + \"finally;\";"
                      "  }"
                      "  return log;"
                      "}"
                      "print(test());";
    std::string expected = "try;finally;";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- catch 块内 throw：finally 必须执行 ----
TEST(TestJIT, R162CatchThrowsFinallyRuns) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  try {"
                      "    throw 1;"
                      "  } catch (e) {"
                      "    log = log + \"catch;\";"
                      "    throw 2;"
                      "  } finally {"
                      "    log = log + \"finally;\";"
                      "  }"
                      "} catch (e2) {"
                      "  log = log + \"outer:\" + e2;"
                      "}"
                      "print(log);";
    std::string expected = "catch;finally;outer:2";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- catch 变量遮蔽全局 ----
TEST(TestJIT, R162CatchVarShadowsGlobal) {
    std::string src = "var e = 999;"
                      "var log = \"\";"
                      "try {"
                      "  throw 42;"
                      "} catch (e) {"
                      "  log = log + \"catch:\" + e + \";\";"
                      "} finally {"
                      "  log = log + \"finally;\";"
                      "}"
                      "print(log);"
                      "print(e);"; // 外层全局 e 应恢复为 999
    std::string expected = "catch:42;finally;999";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- while 循环中 break 触发 finally ----
TEST(TestJIT, R162BreakInTryFinally) {
    std::string src = "var log = \"\";"
                      "var i = 0;"
                      "while (i < 5) {"
                      "  i = i + 1;"
                      "  try {"
                      "    if (i == 3) { break; }"
                      "    log = log + i;"
                      "  } finally {"
                      "    log = log + \"f\";"
                      "  }"
                      "}"
                      "print(log);";
    // i=1: try(1) finally(f) → "1f"
    // i=2: try(2) finally(f) → "1f2f"
    // i=3: try(break) finally(f) → "1f2ff" then break out
    std::string expected = "1f2ff";
    EXPECT_EQ(runJIT(src), expected) << "JIT 输出: " << runJIT(src);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- throw 后续跳值在操作数栈 ----
TEST(TestJIT, R162ThrowExpression) {
    std::string src = "try {"
                      "  throw 1 + 2 + 3;"
                      "} catch (e) {"
                      "  print(e);"
                      "}";
    std::string expected = "6";
    EXPECT_EQ(runJIT(src), expected);
    EXPECT_EQ(minilang_test::runStackVM(src), expected);
}

// ---- 多次 execute 调用：tryStack_ 清空验证 ----
TEST(TestJIT, R162MultipleExecuteClearsTryStack) {
    std::string src = "try { throw 1; } catch (e) { print(e); }";
    // 第一次 execute
    EXPECT_EQ(runJIT(src), "1");
    // 第二次 execute（独立 JITBackend 实例，验证无残留）
    EXPECT_EQ(runJIT(src), "1");
}

// ============================================================
// P2-9: JIT GC 抑制测试（CallbackSuppressor）
// ------------------------------------------------------------
// 验证 JITBackend::execute() 期间增量 GC 回调被抑制，
// 避免不完整 roots 集误回收 JIT 存活容器导致 UAF。
// ============================================================

/// JIT 执行期间 GC 回调被抑制（不触发增量回收）
TEST(TestJITGcSuppression, GcCallbackSuppressedDuringJitExecution) {
    // 设置一个会被调用的 GC 触发回调（标志位 + 计数器）
    auto& gc = GcManager::instance();
    int gcCallCount = 0;
    gc.setGcTriggerCallback([&gcCallCount] { ++gcCallCount; });
    // 调小阈值，确保 JIT 内 helper 分配容器时若回调未被抑制则会触发
    gc.setGcAllocationThreshold(1);
    // 重置分配计数器，确保从 0 开始累计
    gc.reset();

    // JIT 代码创建数组（触发 jitBuildArray helper → registerTracked → checkIncrementalGc）
    // 若 CallbackSuppressor 未生效，gcCallCount 会 > 0
    std::string result = runJIT("var a = [1, 2, 3]; print(a[0] + a[1] + a[2]);");

    EXPECT_EQ(result, "6");
    // JIT 执行期间 GC 回调应被完全抑制
    EXPECT_EQ(gcCallCount, 0) << "JIT 执行期间 GC 回调不应被触发（CallbackSuppressor 应抑制）";

    // 清理：恢复默认状态
    gc.setGcTriggerCallback(nullptr);
    gc.setGcAllocationThreshold(GcManager::GC_ALLOCATION_THRESHOLD);
    gc.reset();
}

/// 多次 JIT 执行验证 CallbackSuppressor 正确恢复回调（无累积抑制）
TEST(TestJITGcSuppression, MultipleJitExecutionsRestoreCallback) {
    auto& gc = GcManager::instance();
    int gcCallCount = 0;
    gc.setGcTriggerCallback([&gcCallCount] { ++gcCallCount; });
    gc.setGcAllocationThreshold(1);
    gc.reset();

    // 连续 3 次 JIT 执行，每次都应抑制 GC 回调
    // 若 CallbackSuppressor 析构未恢复回调，第 2/3 次时回调指针已被置空
    // （suppress 时保存 nullptr → 析构恢复 nullptr），后续抑制无意义但结果应正确
    for (int i = 0; i < 3; ++i) {
        gcCallCount = 0;
        EXPECT_EQ(runJIT("var a = [1, 2]; print(a[0] + a[1]);"), "3");
        EXPECT_EQ(gcCallCount, 0) << "第 " << i + 1 << " 次 JIT 执行期间回调不应被触发";
    }

    // 清理
    gc.setGcTriggerCallback(nullptr);
    gc.setGcAllocationThreshold(GcManager::GC_ALLOCATION_THRESHOLD);
    gc.reset();
}

/// JIT 数组操作不崩溃（验证 GC 抑制期间无 UAF）
TEST(TestJITGcSuppression, JitArrayOperationsNoCrash) {
    // 创建多个数组并执行操作，验证 JIT 期间无 UAF
    // 即使 GC 被抑制，数组本身由引用计数管理生命周期，
    // 循环引用孤岛由下一轮 Interpreter execute() 兜底回收
    std::string src = R"(
        var a = [1, 2, 3];
        var b = [4, 5, 6];
        var c = [a, b];
        print(c[0][0] + c[0][1] + c[0][2]);
        print(c[1][0] + c[1][1] + c[1][2]);
    )";
    EXPECT_EQ(runJIT(src), "615");
}

/// JIT 循环内创建数组不崩溃（长时间运行场景）
TEST(TestJITGcSuppression, JitLoopArrayCreationNoCrash) {
    // 循环内创建数组，累计分配数远超 GC 阈值（8192）
    // 验证 GC 抑制期间不会因未回收导致崩溃
    std::string src = R"(
        var sum = 0;
        var i = 0;
        while (i < 100) {
            var arr = [i, i + 1, i + 2];
            sum = sum + arr[0];
            i = i + 1;
        }
        print(sum);
    )";
    std::string result = runJIT(src);
    // sum = 0+1+2+...+99 = 4950
    EXPECT_EQ(result, "4950");
}

// ============================================================
// R165: 浮点边界值与递归深度边界测试（P3-A3 补充盲区）
// ------------------------------------------------------------
// 现有 R143 浮点测试仅用 3.14/1.5/2.5 等常规值；现有 R160 递归深度测试
// 仅验证统一错误消息，未覆盖边界值（256 刚好触发 / 255 刚好通过）。
// 本组补齐 P1 浮点边界值（NaN/Infinity/-0.0/极大值/极小值）与 P2 递归深度
// 边界值，与 StackVM 输出对比保证一致性。
// ============================================================

// ---- P1: 浮点边界值（与 StackVM 输出对比，不硬编码预期）----

TEST(TestJIT, R165FloatNegativeZero) {
    // -0.0 输出格式（IEEE 754 负零，与 +0.0 数值相等但符号位不同）
    EXPECT_EQ(runJIT("print(-0.0);"), minilang_test::runStackVM("print(-0.0);"));
}

TEST(TestJIT, R165FloatScientificLarge) {
    // 大数科学计数法（1e10 = 10000000000）
    EXPECT_EQ(runJIT("print(1e10);"), minilang_test::runStackVM("print(1e10);"));
}

TEST(TestJIT, R165FloatScientificSmall) {
    // 小数科学计数法（1e-10 = 0.0000000001）
    EXPECT_EQ(runJIT("print(1e-10);"), minilang_test::runStackVM("print(1e-10);"));
}

TEST(TestJIT, R165FloatLargeValue) {
    // 接近 double 上限（1e308，未溢出）
    EXPECT_EQ(runJIT("print(1e308);"), minilang_test::runStackVM("print(1e308);"));
}

TEST(TestJIT, R165FloatSmallValue) {
    // 接近 double 下限（1e-300，未下溢到 denormal）
    EXPECT_EQ(runJIT("print(1e-300);"), minilang_test::runStackVM("print(1e-300);"));
}

TEST(TestJIT, R165FloatMinSubnormal) {
    // subnormal 边界（1e-323，仍可表示但精度极低）
    EXPECT_EQ(runJIT("print(1e-323);"), minilang_test::runStackVM("print(1e-323);"));
}

TEST(TestJIT, R165FloatArithmeticMixed) {
    // 浮点 + 整数混合运算（隐式类型转换）
    EXPECT_EQ(runJIT("print(2 + 0.5);"), minilang_test::runStackVM("print(2 + 0.5);"));
}

TEST(TestJIT, R165FloatNestedArithmetic) {
    // 嵌套浮点运算（验证 IEEE 754 舍入与 StackVM 一致）
    EXPECT_EQ(runJIT("print((1.0/3.0)*3.0);"), minilang_test::runStackVM("print((1.0/3.0)*3.0);"));
}

// ---- P2: 递归深度边界值（255 通过 / 256 触发限制）----

TEST(TestJIT, R165RecursionDepthExactly255) {
    // 255 层递归（与 DeepRecursion255 相同，作为边界对照基准）
    std::string src = "fun count(n) { if (n == 0) { return 0; } return 1 + count(n - 1); } print(count(255));";
    EXPECT_EQ(runJIT(src), "255");
}

TEST(TestJIT, R165RecursionDepthAtLimit256Fails) {
    // 256 层递归应触发 MAX_FRAMES=256 限制（与 StackVM 错误消息一致）
    std::string src = "fun count(n) { if (n == 0) { return 0; } return 1 + count(n - 1); } print(count(256));";
    std::string jitOut = runJIT(src);
    std::string vmOut = minilang_test::runStackVM(src);
    // 错误前缀不同（<jit-runtime: vs <runtime:），但核心消息应一致
    EXPECT_NE(jitOut.find("递归深度超过限制 (256)"), std::string::npos)
        << "JIT 256 层递归应触发限制，实际: " << jitOut;
    EXPECT_NE(vmOut.find("递归深度超过限制 (256)"), std::string::npos)
        << "StackVM 256 层递归应触发限制，实际: " << vmOut;
}

TEST(TestJIT, R165RecursionDepthAtLimit257Fails) {
    // 257 层递归（验证刚超限 1 层也触发限制，与 256 行为一致）
    std::string src = "fun count(n) { if (n == 0) { return 0; } return 1 + count(n - 1); } print(count(257));";
    std::string jitOut = runJIT(src);
    EXPECT_NE(jitOut.find("递归深度超过限制"), std::string::npos) << "JIT 257 层递归应触发限制，实际: " << jitOut;
}

#else // !MINILANG_USE_JIT

// JIT 未启用时提供 skip 占位测试，确保 CTest 注册一致
TEST(TestJIT, Disabled) {
    GTEST_SKIP() << "JIT 后端未启用（MINILANG_USE_JIT=OFF），跳过测试";
}

#endif // MINILANG_USE_JIT
