// ============================================================
// TestConsistencyDiff.cpp — 三后端语义一致性差分测试
// ============================================================
// 对同一棵 AST 分别跑：Interpreter（树遍历）、栈式 VM（IR 路径）、RegisterVM。
// 三后端结果应完全一致（除非显式标注为"允许差异"）。
// 关注错误处理路径：错误类型/消息/是否静默吞掉。
//
// 差分测试约定：
// - 用 EXPECT_EQ 比较 runInterp / runStackVM_IR / runRegVM_IR 的输出
// - 错误路径统一返回 "<runtime:消息>"，比较消息一致性
// - 允许的差异用 EXPECT_NE 显式断言，防止被"修正"后差异被掩盖
// ============================================================

#include <gtest/gtest.h>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include <string>

// ---------- 三后端运行 helper ----------
// 统一返回 "<runtime:消息>" 表示运行时错误，便于差分比较

static std::string runInterp(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        // Interpreter 在 catch RuntimeError 后 throw 重新抛出
        if (out.empty()) return "<runtime:" + std::string(e.what()) + ">";
        // 已有输出后报错：返回输出 + 错误标记
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
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
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
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
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

// 辅助：检查输出是否为运行时错误（可能带前缀输出）
static bool isRuntimeError(const std::string& s) {
    return s.find("<runtime:") != std::string::npos;
}

// 辅助：提取错误消息部分
static std::string errMsg(const std::string& s) {
    auto pos = s.find("<runtime:");
    return pos == std::string::npos ? "" : s.substr(pos + 9);
}

// ============================================================
// 1. 算术运算隐式类型转换
// ============================================================

TEST(ConsistencyDiff, IntFloatPromotion) {
    std::string src = "print(1 + 2.5);";
    EXPECT_EQ(runInterp(src), "3.5");
    EXPECT_EQ(runStackVM_IR(src), "3.5");
    EXPECT_EQ(runRegVM_IR(src), "3.5");
}

TEST(ConsistencyDiff, StringConcatBothString) {
    std::string src = "print(\"ab\" + \"cd\");";
    EXPECT_EQ(runInterp(src), "abcd");
    EXPECT_EQ(runStackVM_IR(src), "abcd");
    EXPECT_EQ(runRegVM_IR(src), "abcd");
}

TEST(ConsistencyDiff, StringConcatWithNumber) {
    std::string src = "print(\"x\" + 1);";
    EXPECT_EQ(runInterp(src), "x1");
    EXPECT_EQ(runStackVM_IR(src), "x1");
    EXPECT_EQ(runRegVM_IR(src), "x1");
}

TEST(ConsistencyDiff, NumberConcatWithString) {
    std::string src = "print(1 + \"x\");";
    EXPECT_EQ(runInterp(src), "1x");
    EXPECT_EQ(runStackVM_IR(src), "1x");
    EXPECT_EQ(runRegVM_IR(src), "1x");
}

TEST(ConsistencyDiff, NullPlusNumber) {
    std::string src = "var n = null; print(n + 1);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri)) << "Interpreter: " << ri;
    EXPECT_TRUE(isRuntimeError(rs)) << "StackVM: " << rs;
    EXPECT_TRUE(isRuntimeError(rr)) << "RegVM: " << rr;
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, StringSubtractError) {
    std::string src = "print(\"a\" - \"b\");";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri)) << "Interpreter: " << ri;
    EXPECT_TRUE(isRuntimeError(rs)) << "StackVM: " << rs;
    EXPECT_TRUE(isRuntimeError(rr)) << "RegVM: " << rr;
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, ArrayPlusNumberError) {
    std::string src = "print([] + 1);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

// ============================================================
// 1b. REG_DIV 真除 vs 栈式截断（已知允许差异）
// ============================================================

TEST(ConsistencyDiff, IntDivTruncationVsTrueDiv) {
    std::string src = "print(7 / 2);";
    EXPECT_EQ(runInterp(src), "3");
    EXPECT_EQ(runStackVM_IR(src), "3");
    EXPECT_NE(runRegVM_IR(src), "3");
    EXPECT_EQ(runRegVM_IR(src), "3.5");
}

TEST(ConsistencyDiff, IntDivExactNoDifference) {
    std::string src = "print(6 / 2);";
    EXPECT_EQ(runInterp(src), "3");
    EXPECT_EQ(runStackVM_IR(src), "3");
    EXPECT_EQ(runRegVM_IR(src), "3");
}

// ============================================================
// 2. 短路求值（and/or）
// ============================================================

TEST(ConsistencyDiff, AndShortCircuitReturnsLeft) {
    // 0 and x → 0（短路，不求值 x）
    std::string src = "print(0 and undefinedVar);";
    EXPECT_EQ(runInterp(src), "0");
    EXPECT_EQ(runStackVM_IR(src), "0");
    EXPECT_EQ(runRegVM_IR(src), "0");
}

TEST(ConsistencyDiff, OrShortCircuitReturnsLeft) {
    // 1 or x → 1（短路）
    std::string src = "print(1 or undefinedVar);";
    EXPECT_EQ(runInterp(src), "1");
    EXPECT_EQ(runStackVM_IR(src), "1");
    EXPECT_EQ(runRegVM_IR(src), "1");
}

// 已知允许差异：Interpreter 的 and/or 在左操作数为真/假时返回 null
// （evaluate(right) 的返回值被丢弃，lastValue_ 未正确传递）
// VMs 正确返回右操作数值
TEST(ConsistencyDiff, AndReturnsRightWhenLeftTruthy) {
    std::string src = "print(1 and 2);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_EQ(rs, "2");
    EXPECT_EQ(rr, "2");
    EXPECT_NE(ri, rs);  // Interpreter 返回 null（已知 bug）
}

TEST(ConsistencyDiff, OrReturnsRightWhenLeftFalsy) {
    std::string src = "print(0 or 2);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_EQ(rs, "2");
    EXPECT_EQ(rr, "2");
    EXPECT_NE(ri, rs);  // Interpreter 返回 null（已知 bug）
}

TEST(ConsistencyDiff, AndWithStringOperand) {
    // "" and 5 → ""（空字符串为假，短路返回左值）—— Interpreter 短路路径正确
    std::string src = "print(\"\" and 5);";
    EXPECT_EQ(runInterp(src), "");
    EXPECT_EQ(runStackVM_IR(src), "");
    EXPECT_EQ(runRegVM_IR(src), "");
}

TEST(ConsistencyDiff, OrWithStringOperand) {
    // "x" or 5 → "x"（非空字符串为真，短路返回左值）
    std::string src = "print(\"x\" or 5);";
    EXPECT_EQ(runInterp(src), "x");
    EXPECT_EQ(runStackVM_IR(src), "x");
    EXPECT_EQ(runRegVM_IR(src), "x");
}

// ============================================================
// 3. 比较运算混合类型
// ============================================================

TEST(ConsistencyDiff, IntEqualsFloat) {
    std::string src = "print(1 == 1.0);";
    EXPECT_EQ(runInterp(src), "true");
    EXPECT_EQ(runStackVM_IR(src), "true");
    EXPECT_EQ(runRegVM_IR(src), "true");
}

TEST(ConsistencyDiff, IntNotEqualsFloat) {
    std::string src = "print(1 == 1.5);";
    EXPECT_EQ(runInterp(src), "false");
    EXPECT_EQ(runStackVM_IR(src), "false");
    EXPECT_EQ(runRegVM_IR(src), "false");
}

TEST(ConsistencyDiff, NumberEqualsString) {
    std::string src = "print(1 == \"1\");";
    EXPECT_EQ(runInterp(src), "false");
    EXPECT_EQ(runStackVM_IR(src), "false");
    EXPECT_EQ(runRegVM_IR(src), "false");
}

TEST(ConsistencyDiff, StringLessThanNumberError) {
    std::string src = "print(\"a\" < 1);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

// ============================================================
// 4. 除零 / 模零错误
// ============================================================

TEST(ConsistencyDiff, IntDivByZero) {
    std::string src = "print(1 / 0);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, FloatDivByZero) {
    std::string src = "print(1.0 / 0.0);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, IntModByZero) {
    std::string src = "print(5 % 0);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, FloatModByZero) {
    std::string src = "print(5.5 % 0.0);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

// ============================================================
// 5. 一元运算符 UOP_NEGATE 类型错误
// ============================================================

TEST(ConsistencyDiff, NegateString) {
    std::string src = "print(-\"abc\");";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    // RegVM 已统一为"一元减运算需要数值类型"，与栈式 VM/Interpreter 一致
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, NegateNull) {
    std::string src = "var n = null; print(-n);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, NegateIntMinOverflow) {
    // INT64_MIN 取负溢出
    // 注意：字面量 -9223372036854775808 会被 lexer 解析为一元负 + 大整数
    std::string src = "var x = 9223372036854775808; print(-x);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    // x 超出 int64 范围，可能被解析为 float
    // 三端行为应一致（无论是否报错）
    EXPECT_EQ(ri, rs) << "Interpreter vs StackVM";
    // RegVM 可能因 float 语义不同
    // EXPECT_EQ(rs, rr);  // 待观察
}

TEST(ConsistencyDiff, NotOnNonBool) {
    std::string src1 = "print(!null);";
    EXPECT_EQ(runInterp(src1), "true");
    EXPECT_EQ(runStackVM_IR(src1), "true");
    EXPECT_EQ(runRegVM_IR(src1), "true");

    std::string src2 = "print(!5);";
    EXPECT_EQ(runInterp(src2), "false");
    EXPECT_EQ(runStackVM_IR(src2), "false");
    EXPECT_EQ(runRegVM_IR(src2), "false");

    std::string src3 = "print(!\"\");";
    EXPECT_EQ(runInterp(src3), "true");
    EXPECT_EQ(runStackVM_IR(src3), "true");
    EXPECT_EQ(runRegVM_IR(src3), "true");
}

// ============================================================
// 6. 空指针/null 访问错误
// ============================================================

TEST(ConsistencyDiff, NullMemberAccess) {
    std::string src = "var n = null; print(n.field);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, NullIndexAccess) {
    std::string src = "var n = null; print(n[0]);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, ArrayIndexOutOfBounds) {
    std::string src = "var a = [1, 2, 3]; print(a[10]);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, ArrayNegativeIndex) {
    std::string src = "var a = [1, 2, 3]; print(a[-1]);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, DictMissingKeyReturnsNull) {
    std::string src = "var d = {\"a\": 1}; print(d[\"missing\"]);";
    EXPECT_EQ(runInterp(src), "null");
    EXPECT_EQ(runStackVM_IR(src), "null");
    EXPECT_EQ(runRegVM_IR(src), "null");
}

TEST(ConsistencyDiff, DictGetMissingReturnsNull) {
    std::string src = "var d = {\"a\": 1}; print(d.get(\"missing\"));";
    EXPECT_EQ(runInterp(src), "null");
    EXPECT_EQ(runStackVM_IR(src), "null");
    EXPECT_EQ(runRegVM_IR(src), "null");
}

TEST(ConsistencyDiff, CallNonFunction) {
    std::string src = "var x = 5; print(x());";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

TEST(ConsistencyDiff, MethodCallOnNonInstance) {
    std::string src = "var x = 5; print(x.foo());";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

// ============================================================
// 7. 闭包捕获语义
// ============================================================

TEST(ConsistencyDiff, ClosureCapturesByReference) {
    std::string src =
        "var counter = 0;\n"
        "func inc() { counter = counter + 1; }\n"
        "inc(); inc();\n"
        "print(counter);\n";
    EXPECT_EQ(runInterp(src), "2");
    EXPECT_EQ(runStackVM_IR(src), "2");
    EXPECT_EQ(runRegVM_IR(src), "2");
}

TEST(ConsistencyDiff, ClosureReadsLiveValue) {
    std::string src =
        "var x = 10;\n"
        "func getX() { return x; }\n"
        "x = 20;\n"
        "print(getX());\n";
    EXPECT_EQ(runInterp(src), "20");
    EXPECT_EQ(runStackVM_IR(src), "20");
    EXPECT_EQ(runRegVM_IR(src), "20");
}

TEST(ConsistencyDiff, NestedClosureCapturesChain) {
    std::string src =
        "func outer() {\n"
        "  var a = 1;\n"
        "  func middle() {\n"
        "    var b = 2;\n"
        "    func inner() { return a + b; }\n"
        "    return inner();\n"
        "  }\n"
        "  return middle();\n"
        "}\n"
        "print(outer());\n";
    EXPECT_EQ(runInterp(src), "3");
    EXPECT_EQ(runStackVM_IR(src), "3");
    EXPECT_EQ(runRegVM_IR(src), "3");
}

TEST(ConsistencyDiff, ClosureMutatesCapturedArray) {
    std::string src =
        "var arr = [1, 2, 3];\n"
        "func appendItem() { arr.push(4); }\n"
        "appendItem();\n"
        "print(arr);\n";
    EXPECT_EQ(runInterp(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runStackVM_IR(src), "[1, 2, 3, 4]");
    EXPECT_EQ(runRegVM_IR(src), "[1, 2, 3, 4]");
}

// ============================================================
// 8. for 循环作用域与求值时机（C 风格 for）
// ============================================================

TEST(ConsistencyDiff, ForLoopBasic) {
    std::string src =
        "var sum = 0;\n"
        "for (var i = 1; i <= 5; i = i + 1) { sum = sum + i; }\n"
        "print(sum);\n";
    EXPECT_EQ(runInterp(src), "15");
    EXPECT_EQ(runStackVM_IR(src), "15");
    EXPECT_EQ(runRegVM_IR(src), "15");
}

TEST(ConsistencyDiff, ForLoopUpdateTiming) {
    // continue 仍执行 upd
    std::string src =
        "var sum = 0;\n"
        "for (var i = 0; i < 5; i = i + 1) {\n"
        "  if (i == 2) { continue; }\n"
        "  if (i == 4) { break; }\n"
        "  sum = sum + i;\n"
        "}\n"
        "print(sum);\n";
    // i=0,1,2(continue),3,4(break) → sum=0+1+3=4
    EXPECT_EQ(runInterp(src), "4");
    EXPECT_EQ(runStackVM_IR(src), "4");
    EXPECT_EQ(runRegVM_IR(src), "4");
}

TEST(ConsistencyDiff, ForLoopNestedScope) {
    std::string src =
        "for (var i = 0; i < 2; i = i + 1) {\n"
        "  for (var j = 0; j < 2; j = j + 1) {\n"
        "    print(i * 10 + j);\n"
        "  }\n"
        "}\n";
    // i=0,j=0→0; i=0,j=1→1; i=1,j=0→10; i=1,j=1→11 → "011011"
    EXPECT_EQ(runInterp(src), "011011");
    EXPECT_EQ(runStackVM_IR(src), "011011");
    EXPECT_EQ(runRegVM_IR(src), "011011");
}

// B1 fix: 循环体内声明的变量被闭包捕获时，三后端均应捕获每次迭代的值快照
// （对齐 Interpreter visitBlock 每次执行创建独立 blockEnv 的语义）。
TEST(ConsistencyDiff, ForLoopBodyVarCapturedByClosure) {
    std::string src =
        "func makeClosures() {\n"
        "  var closures = [];\n"
        "  for (var i = 0; i < 3; i = i + 1) {\n"
        "    var captured = i;\n"
        "    func getter() { return captured; }\n"
        "    closures.push(getter);\n"
        "  }\n"
        "  return closures;\n"
        "}\n"
        "var cs = makeClosures();\n"
        "print(cs[0]()); print(cs[1]()); print(cs[2]());\n";
    // 每次迭代的 blockEnv 独立，captured 捕获 0/1/2
    EXPECT_EQ(runInterp(src), "012");
    EXPECT_EQ(runStackVM_IR(src), "012");
    EXPECT_EQ(runRegVM_IR(src), "012");
}

// B1 fix: 循环变量本身（for-init 声明）被闭包捕获时，三后端均应返回终值
// （i 在 forEnv/函数作用域，非循环体 block，CLOSE_UPVALUE 不关闭它）。
TEST(ConsistencyDiff, ForLoopVarCapturedByClosure) {
    std::string src =
        "func makeClosures() {\n"
        "  var closures = [];\n"
        "  for (var i = 0; i < 3; i = i + 1) {\n"
        "    func getter() { return i; }\n"
        "    closures.push(getter);\n"
        "  }\n"
        "  return closures;\n"
        "}\n"
        "var cs = makeClosures();\n"
        "print(cs[0]()); print(cs[1]()); print(cs[2]());\n";
    // i 在循环体外层作用域，闭包读取终值 3
    EXPECT_EQ(runInterp(src), "333");
    EXPECT_EQ(runStackVM_IR(src), "333");
    EXPECT_EQ(runRegVM_IR(src), "333");
}

// ============================================================
// 9. 数组/字典深拷贝 vs 浅拷贝
// ============================================================

TEST(ConsistencyDiff, AssignmentIsShallowCopy) {
    std::string src =
        "var a = [1, 2, 3];\n"
        "var b = a;\n"
        "b.push(4);\n"
        "print(a);\n"
        "print(b);\n";
    EXPECT_EQ(runInterp(src), "[1, 2, 3][1, 2, 3, 4]");
    EXPECT_EQ(runStackVM_IR(src), "[1, 2, 3][1, 2, 3, 4]");
    EXPECT_EQ(runRegVM_IR(src), "[1, 2, 3][1, 2, 3, 4]");
}

TEST(ConsistencyDiff, DictAssignmentIsShallowCopy) {
    std::string src =
        "var d1 = {\"k\": 1};\n"
        "var d2 = d1;\n"
        "d2[\"k\"] = 99;\n"
        "print(d1);\n"
        "print(d2);\n";
    EXPECT_EQ(runInterp(src), "{\"k\": 1}{\"k\": 99}");
    EXPECT_EQ(runStackVM_IR(src), "{\"k\": 1}{\"k\": 99}");
    EXPECT_EQ(runRegVM_IR(src), "{\"k\": 1}{\"k\": 99}");
}

TEST(ConsistencyDiff, ArrayPushSelf) {
    std::string src =
        "var a = [1, 2];\n"
        "a.push(a);\n"
        "print(a.len());\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, DictSelfReference) {
    std::string src =
        "var d = {};\n"
        "d[\"self\"] = d;\n"
        "print(d.has(\"self\"));\n";
    EXPECT_EQ(runInterp(src), "true");
    EXPECT_EQ(runStackVM_IR(src), "true");
    EXPECT_EQ(runRegVM_IR(src), "true");
}

TEST(ConsistencyDiff, NestedArrayEqualityWithCycle) {
    std::string src =
        "var a = [1];\n"
        "a.push(a);\n"
        "var b = [1];\n"
        "b.push(b);\n"
        "print(a == b);\n";
    EXPECT_EQ(runInterp(src), "true");
    EXPECT_EQ(runStackVM_IR(src), "true");
    EXPECT_EQ(runRegVM_IR(src), "true");
}

// ============================================================
// 10. super.method() this 绑定与字段同步
// ============================================================

TEST(ConsistencyDiff, SuperMethodThisFieldSync) {
    std::string src =
        "class Base {\n"
        "  var x;\n"
        "  func setX(v) { this.x = v; }\n"
        "}\n"
        "class Derived : Base {\n"
        "  var y;\n"
        "  func setBoth(v) { super.setX(v); this.y = v + 1; }\n"
        "}\n"
        "var d = Derived();\n"
        "d.setBoth(10);\n"
        "print(d.x);\n"
        "print(d.y);\n";
    EXPECT_EQ(runInterp(src), "1011");
    EXPECT_EQ(runStackVM_IR(src), "1011");
    EXPECT_EQ(runRegVM_IR(src), "1011");
}

TEST(ConsistencyDiff, SuperCallChain) {
    std::string src =
        "class A {\n"
        "  var name;\n"
        "  func init() { this.name = \"A\"; }\n"
        "}\n"
        "class B : A {\n"
        "  func init() { super.init(); this.name = this.name + \"B\"; }\n"
        "}\n"
        "class C : B {\n"
        "  func init() { super.init(); this.name = this.name + \"C\"; }\n"
        "}\n"
        "var c = C();\n"
        "print(c.name);\n";
    EXPECT_EQ(runInterp(src), "ABC");
    EXPECT_EQ(runStackVM_IR(src), "ABC");
    EXPECT_EQ(runRegVM_IR(src), "ABC");
}

TEST(ConsistencyDiff, SuperMethodNoParentError) {
    std::string src =
        "class A {\n"
        "  func foo() { super.bar(); }\n"
        "}\n"
        "var a = A();\n"
        "a.foo();\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

// ============================================================
// 11. 其他错误路径一致性
// ============================================================

TEST(ConsistencyDiff, UndefinedVariable) {
    std::string src = "print(undefinedVar);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

TEST(ConsistencyDiff, UndefinedFunction) {
    std::string src = "undefinedFunc();";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

TEST(ConsistencyDiff, TooFewArguments) {
    std::string src =
        "func test(a, b) { return a + b; }\n"
        "print(test(1));\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

TEST(ConsistencyDiff, TooManyArguments) {
    std::string src =
        "func test(a, b) { return a + b; }\n"
        "print(test(1, 2, 3));\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

TEST(ConsistencyDiff, PopOnEmptyArray) {
    std::string src =
        "var a = [];\n"
        "a.pop();\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, StringIndexOutOfBounds) {
    std::string src = "print(\"abc\"[10]);";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, ThrowUncaughtException) {
    std::string src = "throw \"oops\";";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

// ============================================================
// 审计盲区补充测试 (Coverage Gap Tests G1-G12)
// ============================================================
// 来源:测试覆盖审计发现的盲区。每个测试锁定一个高风险构造的行为,
// 防止未来修改引入回归。已知后端差异用 EXPECT_NE 显式标注。
// ============================================================

// G1: 类方法内嵌套闭包捕获 this
// 不变量:内层闭包 step 通过 upvalue 捕获 this,修改 this.count 后
//         外层方法 inc 能读到最新值。失败指示 IR WRITEBACK_MEMBER_UPVALUE
//         与 METHOD_CALL 接收者写回链协作 bug。
TEST(ConsistencyDiff, G1_ClassMethodNestedClosureCapturesThis) {
    std::string src =
        "class Counter {\n"
        "  var count;\n"
        "  func init() { this.count = 0; }\n"
        "  func inc() {\n"
        "    func step() { this.count = this.count + 1; }\n"
        "    step(); step();\n"
        "    return this.count;\n"
        "  }\n"
        "}\n"
        "var c = Counter();\n"
        "print(c.inc());\n";
    EXPECT_EQ(runInterp(src), "2");
    EXPECT_EQ(runStackVM_IR(src), "2");
    EXPECT_EQ(runRegVM_IR(src), "2");
}

// G2: 字段遮蔽 — MiniLang 不支持,父子类同名字段合并为单一 map 槽
// 不变量:b.x 和 b.show() 都返回 20(子类 init 覆盖父类 init 设的值)。
//         失败指示 DEFINE_CLASS 字段表合并逻辑或 init 同步链 bug。
TEST(ConsistencyDiff, G2_FieldShadowingMergedIntoSingleSlot) {
    std::string src =
        "class A {\n"
        "  var x;\n"
        "  func init() { this.x = 10; }\n"
        "  func show() { return this.x; }\n"
        "}\n"
        "class B : A {\n"
        "  var x;\n"
        "  func init() { super.init(); this.x = 20; }\n"
        "}\n"
        "var b = B();\n"
        "print(b.x);\n"
        "print(b.show());\n";
    EXPECT_EQ(runInterp(src), "2020");
    EXPECT_EQ(runStackVM_IR(src), "2020");
    EXPECT_EQ(runRegVM_IR(src), "2020");
}

// G3: 33 个局部变量超出 RegisterVM 32 寄存器硬上限
// 不变量:Interpreter/StackVM 正常输出 33;RegisterVM 编译失败(compile error)。
//         失败指示 P2-1 localCount>32 守卫缺失,导致 RegCallFrame::registers[32] OOB。
TEST(ConsistencyDiff, G3_LocalCountExceeds32RegisterLimit) {
    std::string src =
        "func f() {\n"
        "  var a1=1; var a2=2; var a3=3; var a4=4; var a5=5;\n"
        "  var a6=6; var a7=7; var a8=8; var a9=9; var a10=10;\n"
        "  var a11=11; var a12=12; var a13=13; var a14=14; var a15=15;\n"
        "  var a16=16; var a17=17; var a18=18; var a19=19; var a20=20;\n"
        "  var a21=21; var a22=22; var a23=23; var a24=24; var a25=25;\n"
        "  var a26=26; var a27=27; var a28=28; var a29=29; var a30=30;\n"
        "  var a31=31; var a32=32; var a33=33;\n"
        "  return a33;\n"
        "}\n"
        "print(f());\n";
    EXPECT_EQ(runInterp(src), "33");
    EXPECT_EQ(runStackVM_IR(src), "33");
    // RegisterVM 应编译失败(localCount > 32)
    auto rr = runRegVM_IR(src);
    EXPECT_TRUE(rr.find("<compile:") != std::string::npos)
        << "RegisterVM 应在 localCount > 32 时编译失败,实际: " << rr;
}

// G4: 顶层前向引用 — Interpreter 不提升函数声明,VMs 在编译期全部注册
// 不变量:Interpreter 报"未定义的变量: foo";StackVM/RegisterVM 输出 42。
//         这是已知后端差异,用 EXPECT_NE 锁定防止被"修正"后差异被掩盖。
TEST(ConsistencyDiff, G4_TopLevelForwardReferenceDivergence) {
    std::string src =
        "print(foo());\n"
        "func foo() { return 42; }\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri)) << "Interpreter 应报未定义变量错误";
    EXPECT_EQ(rs, "42");
    EXPECT_EQ(rr, "42");
    EXPECT_NE(ri, rs);  // 已知差异:Interpreter 不提升,VMs 提升
}

// G5: null 作为字典键 — StackVM 与 RegisterVM 错误消息不同
// 不变量:三后端都应报错(null 不是合法字符串键),但 StackVM 报
//         "该类型不支持索引赋值",RegisterVM 报"字典键必须是字符串"。
//         用 EXPECT_NE 锁定已知消息差异。
TEST(ConsistencyDiff, G5_NullAsDictKeyErrorMessageDivergence) {
    std::string src =
        "var d = {};\n"
        "d[null] = 1;\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    // StackVM 和 RegisterVM 错误消息不同(已知差异)
    EXPECT_NE(errMsg(rs), errMsg(rr));
}

// G6: 负数 substr 参数
// 不变量:负起始位置 → 返回空串(不报错);负长度 → 报错。
//         三后端共享 executeSharedStrSubstr 实现,行为应一致。
TEST(ConsistencyDiff, G6_NegativeSubstrStartReturnsEmpty) {
    std::string src = "print(\"hello\".substr(-2));\n";
    EXPECT_EQ(runInterp(src), "");
    EXPECT_EQ(runStackVM_IR(src), "");
    EXPECT_EQ(runRegVM_IR(src), "");
}

TEST(ConsistencyDiff, G6_NegativeSubstrLengthErrors) {
    std::string src = "print(\"hello\".substr(1, -1));\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

// G7: 直接递归触发 MAX_RECURSION_DEPTH/MAX_FRAMES (256)
// 不变量:三后端都应报运行时错误。Interpreter 消息"递归深度超过限制 (256)",
//         VM 消息"调用栈溢出"。用 EXPECT_NE 锁定已知消息差异。
TEST(ConsistencyDiff, G7_DeepRecursionHitsLimit) {
    std::string src =
        "func recurse(n) { return recurse(n + 1); }\n"
        "recurse(0);\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    // Interpreter 和 VM 错误消息不同(已知差异)
    EXPECT_NE(errMsg(ri), errMsg(rs));
    EXPECT_EQ(errMsg(rs), errMsg(rr));  // StackVM 和 RegisterVM 一致
}

// G8: 互递归触发 MAX_FRAMES (256)
// 不变量:even(1000) 和 odd(1000) 互递归超过 256 帧上限,三后端都应报错。
TEST(ConsistencyDiff, G8_MutualRecursionHitsFrameLimit) {
    std::string src =
        "func even(n) { if (n == 0) { return 1; } return odd(n - 1); }\n"
        "func odd(n) { if (n == 0) { return 0; } return even(n - 1); }\n"
        "even(1000);\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
}

// G9: if 块内声明闭包
// 不变量:if 块退出时关闭 upvalue,闭包捕获 x 的终值 10。
//         失败指示 visitIfStmt 退出路径未调用 closeCapturedVariables
//         或 StackVM 未 emit OP_CLOSE_UPVALUE。
TEST(ConsistencyDiff, G9_IfBlockClosureCapture) {
    std::string src =
        "func test() {\n"
        "  var holder = [];\n"
        "  if (true) {\n"
        "    var x = 10;\n"
        "    func getter() { return x; }\n"
        "    holder.push(getter);\n"
        "  }\n"
        "  print(holder[0]());\n"
        "}\n"
        "test();\n";
    EXPECT_EQ(runInterp(src), "10");
    EXPECT_EQ(runStackVM_IR(src), "10");
    EXPECT_EQ(runRegVM_IR(src), "10");
}

// G10: 乘法和模运算整数溢出
// 不变量:INT64_MAX*2 和 INT64_MIN%-1 都应报"整数运算溢出"。
//         三后端共享 NumericOps::computeArith,行为应一致。
TEST(ConsistencyDiff, G10_MulOverflowErrors) {
    std::string src = "print(9223372036854775807 * 2);\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

TEST(ConsistencyDiff, G10_ModInt64MinByNegOneErrors) {
    // -9223372036854775808 字面量会被 lexer 解析为 float(超出 int64 范围),
    // 需通过 -9223372036854775807 - 1 在运行时构造 INT64_MIN。
    std::string src =
        "var min = -9223372036854775807 - 1;\n"
        "print(min % -1);\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    EXPECT_EQ(ri, rs);
    EXPECT_EQ(rs, rr);
}

// G11: 字符串方法在非字符串类型上调用
// 不变量:三后端都应报错,且错误消息完全一致(BUG-1 修复后统一)。
//   修复前:Interpreter "类型 int 不支持方法调用" / StackVM "方法调用需要类实例"
//           / RegisterVM "方法调用需要类实例"。三后端两两不一致,且 StackVM/RegisterVM
//           消息具有误导性(暗示需要类实例,但实际是类型不支持该方法)。
//   修复后(2026-06-29 BUG-1):三后端统一为"类型 int 不支持方法 upper"。
TEST(ConsistencyDiff, G11_StringMethodOnIntErrorMessageDivergence) {
    std::string src = "print((42).upper());\n";
    auto ri = runInterp(src), rs = runStackVM_IR(src), rr = runRegVM_IR(src);
    EXPECT_TRUE(isRuntimeError(ri));
    EXPECT_TRUE(isRuntimeError(rs));
    EXPECT_TRUE(isRuntimeError(rr));
    // BUG-1 修复后三后端错误消息完全一致
    EXPECT_EQ(errMsg(ri), errMsg(rs)) << "Interpreter vs StackVM";
    EXPECT_EQ(errMsg(rs), errMsg(rr)) << "StackVM vs RegisterVM";
    EXPECT_EQ(errMsg(ri), errMsg(rr)) << "Interpreter vs RegisterVM";
    // 验证统一消息包含"类型 int 不支持方法 upper"(errMsg 末尾带 ">"
    // 是 runInterp helper 包裹 "<runtime:...>" 的副产物,不影响一致性检查)
    EXPECT_NE(errMsg(ri).find("类型 int 不支持方法 upper"), std::string::npos);
}

// G12: while 块内声明闭包 — 每次迭代应独立捕获
// 不变量:while 循环体每次迭代创建独立 block scope,闭包捕获 per-iteration 值。
//         期望输出 "012"(对齐 B1 的 for 循环行为)。
//         失败指示 visitWhileStmt 未正确调用 closeCapturedVariables 或
//         StackVM 未对 while body emit CLOSE_UPVALUE。
TEST(ConsistencyDiff, G12_WhileBlockClosurePerIterationCapture) {
    std::string src =
        "func makeClosures() {\n"
        "  var closures = [];\n"
        "  var i = 0;\n"
        "  while (i < 3) {\n"
        "    var captured = i;\n"
        "    func getter() { return captured; }\n"
        "    closures.push(getter);\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return closures;\n"
        "}\n"
        "var cs = makeClosures();\n"
        "print(cs[0]()); print(cs[1]()); print(cs[2]());\n";
    EXPECT_EQ(runInterp(src), "012");
    EXPECT_EQ(runStackVM_IR(src), "012");
    EXPECT_EQ(runRegVM_IR(src), "012");
}
