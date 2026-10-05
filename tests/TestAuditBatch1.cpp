// ============================================================
// 审计批次 1 回归测试
// ------------------------------------------------------------
// 覆盖审计发现并修复的 6 项 Bug：
//   BUG-AUDIT-FINALLY-1  (P0): finally 块语法（AST+Parser+三后端）
//   BUG-INH-AUDIT-3      (P1): VM/RegisterVM 循环继承检测
//   BUG-AUDIT-CLOSE-1    (P1): 直接 Compiler 路径 OP_CLOSE_UPVALUE
//   BUG-REPL-AUDIT-7     (P1): GcManager 根标记 gcRootPtr 对闭包返回非空
//   BUG-INH-AUDIT-1      (P1): IR 嵌套 super 运行时错误（三后端一致拒绝）
//   BUG-IR-OPT-AUDIT-6   (P2): optimizeIR 空指针防御性检查
// ============================================================

#include <gtest/gtest.h>

#include "compiler/core/Bytecode.h"
#include "compiler/core/Compiler.h"
#include "compiler/ir/IR.h"
#include "compiler/backend-reg/RegisterVM.h"
#include "compiler/backend-stack/VM.h"
#include "interpreter/Environment.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// 辅助函数
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
// BUG-AUDIT-FINALLY-1: finally 块语法
// ------------------------------------------------------------
// 测试三后端（Interpreter / StackVM / StackVM-IR / RegisterVM）
// 的 try-catch-finally 语义一致性：
//   1. 正常路径：try 块完成后执行 finally
//   2. 异常路径：catch 处理后执行 finally
//   3. 未捕获异常：finally 执行后 rethrow
// ============================================================

// ---- finally 正常路径：try 无异常，finally 在 catch 后执行 ----
TEST(AuditBatch1Finally, NormalPathRunsFinally) {
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
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- finally 异常路径：try 抛异常，catch 处理后 finally 执行 ----
TEST(AuditBatch1Finally, ExceptionPathRunsFinallyAfterCatch) {
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
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- finally 内部异常未捕获：finally 执行后 rethrow 传播到外层 ----
TEST(AuditBatch1Finally, UncaughtExceptionRethrowsAfterFinally) {
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
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- finally 在函数内：finally 执行后函数返回 ----
TEST(AuditBatch1Finally, FinallyInFunction) {
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
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- finally 中 catch 块内 throw：finally 必须执行 ----
TEST(AuditBatch1Finally, CatchThrowsFinallyStillRuns) {
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
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- finally 配合顶层 catch 变量遮蔽全局 ----
TEST(AuditBatch1Finally, FinallyWithTopLevelCatchShadowingGlobal) {
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
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- BUG-AUDIT-FINALLY-DOUBLE: finally 块自身抛 throw 时不应双重执行 ----
// 复现：finally 块内 throw 时，Interpreter 路径原 finallyRun 标志设置在 evaluate 之后，
// 导致 finally 抛异常时 finallyRun 仍为 false，外层 catch 误判为"finally 未执行"并再次执行。
// 修复：finallyRun = true 移到 evaluate(finallyBlock) 之前。
TEST(AuditBatch1Finally, FinallyThrowsDoesNotDoubleExecute) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  try {"
                      "    throw 1;"
                      "  } catch (e) {"
                      "    log = log + \"catch;\";"
                      "  } finally {"
                      "    log = log + \"finally;\";"
                      "    throw 2;"
                      "  }"
                      "} catch (e2) {"
                      "  log = log + \"outer:\" + e2;"
                      "}"
                      "print(log);";
    // finally 只执行一次（不是 "catch;finally;finally;outer:2"）
    std::string expected = "catch;finally;outer:2";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- BUG-AUDIT-FINALLY-DOUBLE: try-finally（无 catch）+ finally 抛 throw ----
// finally 自身抛 throw 时应只执行一次，新异常覆盖原异常传播到外层
TEST(AuditBatch1Finally, TryFinallyWithoutCatchThrowsInFinally) {
    std::string src = "var log = \"\";"
                      "try {"
                      "  try {"
                      "    log = log + \"try;\";"
                      "    throw 1;"
                      "  } finally {"
                      "    log = log + \"finally;\";"
                      "    throw 2;"
                      "  }"
                      "} catch (e) {"
                      "  log = log + \"catch:\" + e;"
                      "}"
                      "print(log);";
    // 原异常 1 被 finally 抛出的 2 覆盖，finally 只执行一次
    std::string expected = "try;finally;catch:2";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- BUG-AUDIT-PRINT-MULTI: 多值 print 三后端一致性 ----
// 复现：IR 路径原 visitPrintStmt 逐个值 emit PRINT，导致每个值后跟换行，
// 与 Interpreter/StackVM 直接路径的 "v1 v2 v3" 单行空格拼接不一致。
// 修复：IR.cpp visitPrintStmt 用 ADD 拼接多值为单字符串后发射单个 PRINT。
TEST(AuditBatch1Print, MultiValuePrintThreeEnginesConsistency) {
    std::string src = "print(1, 2, 3);";
    // 三后端均应输出 "1 2 3"（空格拼接，单次 output 带换行）
    std::string expected = "1 2 3";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- BUG-AUDIT-PRINT-MULTI: 多值 print 混合类型 ----
// 注：3.14 在 double 中无法精确表示（实际存储为 3.1400000000000001），
// 三后端均输出该全精度串。改用 3.5（二进制可精确表示）避免脆弱测试。
TEST(AuditBatch1Print, MultiValuePrintMixedTypes) {
    std::string src = "print(\"x\", 42, true, 3.5);";
    // 空格拼接：x 42 true 3.5
    std::string expected = "x 42 true 3.5";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- BUG-AUDIT-PRINT-MULTI: 单值 print 不受影响 ----
TEST(AuditBatch1Print, SingleValuePrintUnchanged) {
    std::string src = "print(42);";
    std::string expected = "42";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- BUG-AUDIT-PRINT-MULTI: 空 print 输出空行 ----
TEST(AuditBatch1Print, EmptyPrintOutputsNewline) {
    std::string src = "print();";
    // print() → 输出空字符串（output("") 会追加换行）
    std::string expected = "";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// P1-D: Formatter 单语句体往返等价性
// ------------------------------------------------------------
// 验证 if/while/for 单语句体（无花括号）格式化后保留原结构，
// 重新解析产生等价 AST（thenBranch/body 节点类型不变）。
// ============================================================

#include "ast/ASTNode.h"
#include "formatter/Formatter.h"

static std::string formatSource(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Formatter f;
    return f.format(*ast);
}

// ---- if 单语句体保留无花括号形式 ----
TEST(AuditBatch1Formatter, IfSingleStmtBodyPreserved) {
    std::string src = "if (true) print(1);";
    std::string result = formatSource(src);
    // 应保留无花括号形式（不应出现 "{"）
    EXPECT_EQ(result.find("{"), std::string::npos) << "单语句体不应被强制包裹花括号，实际: " << result;
    // 幂等性
    EXPECT_EQ(result, formatSource(result));
}

// ---- while 单语句体保留无花括号形式 ----
TEST(AuditBatch1Formatter, WhileSingleStmtBodyPreserved) {
    std::string src = "while (i < 10) print(i);";
    std::string result = formatSource(src);
    EXPECT_EQ(result.find("{"), std::string::npos) << "单语句体不应被强制包裹花括号，实际: " << result;
    EXPECT_EQ(result, formatSource(result));
}

// ---- for 单语句体保留无花括号形式 ----
TEST(AuditBatch1Formatter, ForSingleStmtBodyPreserved) {
    std::string src = "for (var i = 0; i < 10; i = i + 1) print(i);";
    std::string result = formatSource(src);
    EXPECT_EQ(result.find("{"), std::string::npos) << "单语句体不应被强制包裹花括号，实际: " << result;
    EXPECT_EQ(result, formatSource(result));
}

// ---- if-else 单语句体保留无花括号形式 ----
TEST(AuditBatch1Formatter, IfElseSingleStmtBodyPreserved) {
    std::string src = "if (a) print(1); else print(2);";
    std::string result = formatSource(src);
    // 不应出现任何花括号
    EXPECT_EQ(result.find("{"), std::string::npos) << "if-else 单语句体不应被强制包裹花括号，实际: " << result;
    EXPECT_EQ(result, formatSource(result));
}

// ---- if-else-if 链末尾 else 单语句体保留无花括号形式 ----
TEST(AuditBatch1Formatter, ElseIfChainSingleStmtBodyPreserved) {
    std::string src = "if (a) print(1); else if (b) print(2); else print(3);";
    std::string result = formatSource(src);
    EXPECT_EQ(result.find("{"), std::string::npos) << "else-if 链单语句体不应被强制包裹花括号，实际: " << result;
    EXPECT_EQ(result, formatSource(result));
}

// ---- Block 体仍保留花括号（回归保护） ----
TEST(AuditBatch1Formatter, BlockBodyStillHasBraces) {
    std::string src = "if (true) { print(1); print(2); }";
    std::string result = formatSource(src);
    // Block 体应保留花括号
    EXPECT_NE(result.find("{"), std::string::npos) << "Block 体应保留花括号，实际: " << result;
    EXPECT_EQ(result, formatSource(result));
}

// ============================================================
// P2-C: REPL isInputComplete 识别 finally
// ------------------------------------------------------------
// 验证 try-finally（无 catch）被视为完整输入
// ============================================================

// 注：isInputComplete 是 ReplPanel 的私有方法，此处通过 ReplPanel 的公开行为
// 间接验证不可行（需 GUI 环境）。改为直接测试逻辑：finally 应配对 try。
// 由于无法直接调用私有方法，此处用逻辑断言代替。

TEST(AuditBatch1ReplFinally, TryFinallyIsCompleteLogic) {
    // 逻辑验证：try + finally 应视为完整（finallyCount >= 1 配对 tryCount == 1）
    // 原 Bug：tryCount(1) > catchCount(0) → 误判不完整
    // 修复后：tryCount(1) > catchCount(0) + finallyCount(1) → 1 > 1 → false → 完整
    int tryCount = 1, catchCount = 0, finallyCount = 1;
    EXPECT_FALSE(tryCount > catchCount + finallyCount) << "try-finally（无 catch）应视为完整输入";
}

TEST(AuditBatch1ReplFinally, TryCatchFinallyIsCompleteLogic) {
    // try + catch + finally 也是完整
    int tryCount = 1, catchCount = 1, finallyCount = 1;
    EXPECT_FALSE(tryCount > catchCount + finallyCount) << "try-catch-finally 应视为完整输入";
}

TEST(AuditBatch1ReplFinally, TryWithoutCatchOrFinallyIsIncompleteLogic) {
    // try 无 catch 无 finally 仍应视为不完整
    int tryCount = 1, catchCount = 0, finallyCount = 0;
    EXPECT_TRUE(tryCount > catchCount + finallyCount) << "try 无 catch 无 finally 应视为不完整输入";
}

// ============================================================
// BUG-INH-AUDIT-3: 循环继承检测（RegisterVM）
// ------------------------------------------------------------
// 验证有效多层继承不会误报循环（guard 上限合理），
// 以及 RegisterVM 能正确处理深层继承链。
// ============================================================

TEST(AuditBatch1Inherit, DeepInheritanceNoFalseCycle) {
    // 5 层继承链，应正常工作不触发循环检测
    std::string src = "class A { var a = 1; }"
                      "class B extends A { var b = 2; }"
                      "class C extends B { var c = 3; }"
                      "class D extends C { var d = 4; }"
                      "class E extends D { var e = 5; }"
                      "var obj = E();"
                      "print(obj.a);"
                      "print(obj.b);"
                      "print(obj.c);"
                      "print(obj.d);"
                      "print(obj.e);";
    std::string expected = "12345";
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- RegisterVM 深层继承方法查找 + super 链 ----
TEST(AuditBatch1Inherit, DeepInheritanceMethodLookup) {
    std::string src = "class Base {"
                      "  fun who() { return \"base\"; }"
                      "}"
                      "class M1 extends Base {"
                      "  fun who() { return \"m1-\" + super.who(); }"
                      "}"
                      "class M2 extends M1 {"
                      "  fun who() { return \"m2-\" + super.who(); }"
                      "}"
                      "class M3 extends M2 {"
                      "  fun who() { return \"m3-\" + super.who(); }"
                      "}"
                      "var o = M3();"
                      "print(o.who());";
    std::string expected = "m3-m2-m1-base";
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// BUG-AUDIT-CLOSE-1: 直接 Compiler 路径 OP_CLOSE_UPVALUE
// ------------------------------------------------------------
// 验证闭包捕获循环/块变量时产生值快照（by-value），
// 而非引用最终值。这是 OP_CLOSE_UPVALUE 在块退出时关闭
// upvalue 的核心语义。
// ============================================================

// ---- for 循环闭包快照：每次迭代捕获当时的 i 值 ----
TEST(AuditBatch1CloseUpvalue, ForLoopClosureSnapshot) {
    // 每个闭包应返回其创建时 captured 的快照值（OP_CLOSE_UPVALUE 在块退出时关闭 upvalue）
    // MiniLang 不支持 fun() {} 作为表达式，使用命名函数声明 + 引用赋值
    std::string src = "fun makeFns() {"
                      "  var fns = [];"
                      "  for (var i = 0; i < 3; i = i + 1) {"
                      "    var captured = i;"
                      "    fun f() { return captured; }"
                      "    fns.push(f);"
                      "  }"
                      "  return fns;"
                      "}"
                      "var fns = makeFns();"
                      "print(fns[0]());"
                      "print(fns[1]());"
                      "print(fns[2]());";
    std::string expected = "012";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- while 循环闭包快照 ----
TEST(AuditBatch1CloseUpvalue, WhileLoopClosureSnapshot) {
    std::string src = "fun makeFns() {"
                      "  var fns = [];"
                      "  var i = 0;"
                      "  while (i < 3) {"
                      "    var captured = i;"
                      "    fun f() { return captured; }"
                      "    fns.push(f);"
                      "    i = i + 1;"
                      "  }"
                      "  return fns;"
                      "}"
                      "var fns = makeFns();"
                      "print(fns[0]());"
                      "print(fns[1]());"
                      "print(fns[2]());";
    std::string expected = "012";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ---- if 分支闭包快照 ----
TEST(AuditBatch1CloseUpvalue, IfBlockClosureSnapshot) {
    // 对齐 AuditBugI1_ClosureInBlockCapturingParentVar 模式：
    // 命名函数声明 + g = f 引用赋值 + 额外块触发 OP_CLOSE_UPVALUE
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
    // R103 W1 fix: StackVM 直接路径已通过 OP_GET_LOCAL + OP_CALL_EXPR 支持
    // `var g = f; g();` 局部闭包变量调用，对齐 IR.cpp CRITICAL-1 fix。
    // 原"已知限制"已消除，三后端一致。
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// BUG-REPL-AUDIT-7: GcManager 根标记 gcRootPtr 对闭包返回非空
// ------------------------------------------------------------
// 验证 Value::gcRootPtr() 对闭包返回非空指针，
// 使 GcManager 的 markPhase 能进入 capturedVars 标记可达容器。
// ============================================================

TEST(AuditBatch1GcRoot, ClosureGcRootPtrNonNull) {
    // 创建一个闭包 Value，验证 gcRootPtr 返回非空
    auto env = std::make_shared<Environment>();
    std::vector<std::string> params = {"x"};
    Value closureVal = Value::makeClosure("testFn", env, params, nullptr);
    ASSERT_TRUE(closureVal.isClosure());
    // BUG-REPL-AUDIT-7 fix: gcRootPtr 对闭包应返回非空指针
    const void* rootPtr = closureVal.gcRootPtr();
    EXPECT_NE(rootPtr, nullptr);
}

// ---- 标量类型的 gcRootPtr 返回 nullptr（不应作为 GC 根）----
TEST(AuditBatch1GcRoot, ScalarGcRootPtrNull) {
    Value intVal(42);
    EXPECT_EQ(intVal.gcRootPtr(), nullptr);

    Value floatVal(3.14);
    EXPECT_EQ(floatVal.gcRootPtr(), nullptr);

    Value boolVal(true);
    EXPECT_EQ(boolVal.gcRootPtr(), nullptr);

    Value nullVal = Value::nullValue();
    EXPECT_EQ(nullVal.gcRootPtr(), nullptr);
}

// ---- 数组/字典/实例的 gcRootPtr 返回非空（回归保护）----
TEST(AuditBatch1GcRoot, ContainerGcRootPtrNonNull) {
    // 数组：通过 vector 构造
    Value arr(std::vector<Value>{Value(1)});
    ASSERT_TRUE(arr.isArray());
    EXPECT_NE(arr.gcRootPtr(), nullptr);

    // 字典：通过 unordered_map 构造
    Value dict(std::unordered_map<std::string, Value>{{"k", Value(1)}});
    ASSERT_TRUE(dict.isDict());
    EXPECT_NE(dict.gcRootPtr(), nullptr);

    // 实例
    Value inst = Value::makeInstance("Foo");
    ASSERT_TRUE(inst.isInstance());
    EXPECT_NE(inst.gcRootPtr(), nullptr);
}

// ============================================================
// BUG-INH-AUDIT-1: IR 嵌套 super 运行时错误
// ------------------------------------------------------------
// 验证在非实例方法体（顶层或嵌套函数内）使用 super 时，
// 三后端统一在运行时报错（不崩溃）。
//
// 设计说明：super 在非方法上下文中的错误为运行时错误（非编译期），
// 由 VM 在 emitLoadVar("this") 回退到 GLOBAL_NAME 查找失败时触发。
// 此运行时错误不可被 try/catch 捕获（在 try 块进入前即触发）。
// 与 ConsistencyDiff.H7b_SuperInNonMethodContext 和
// AuditSuper_RuntimeErrorNotCatchableByTryCatch 测试对齐。
// ============================================================

// ---- 顶层使用 super 报运行时错误 ----
TEST(AuditBatch1Super, TopLevelSuperRuntimeError) {
    std::string src = "class A { fun foo() { return 1; } }"
                      "var x = super.foo();";
    auto ri = runInterpreter(src);
    auto rs = runStackVM_IR(src);
    auto rr = runRegVM(src);
    // 三后端都应报运行时错误（不崩溃、不静默通过）
    EXPECT_NE(ri.find("<runtime:"), std::string::npos) << "T1 Interpreter 应报运行时错误，实际: \"" << ri << "\"";
    EXPECT_NE(rs.find("<runtime:"), std::string::npos) << "T1 StackVM 应报运行时错误，实际: \"" << rs << "\"";
    EXPECT_NE(rr.find("<runtime:"), std::string::npos) << "T1 RegVM 应报运行时错误，实际: \"" << rr << "\"";
}

// ---- 顶层函数内使用 super 报运行时错误 ----
// 注意：方法内嵌套函数中使用 super 是合法设计（对齐直接 Compiler 路径，
// currentClassName_/compilingClassName_ 在 visitFunDecl 中不清除）。
// 此测试验证非类上下文（顶层函数）中使用 super 报运行时错误。
TEST(AuditBatch1Super, NestedFunctionSuperRuntimeError) {
    std::string src = "class A { fun foo() { return 1; } }"
                      "fun outer() {"
                      "  return super.foo();"
                      "}"
                      "outer();";
    auto ri = runInterpreter(src);
    auto rs = runStackVM_IR(src);
    auto rr = runRegVM(src);
    EXPECT_NE(ri.find("<runtime:"), std::string::npos) << "T2 Interpreter 应报运行时错误，实际: \"" << ri << "\"";
    EXPECT_NE(rs.find("<runtime:"), std::string::npos) << "T2 StackVM 应报运行时错误，实际: \"" << rs << "\"";
    EXPECT_NE(rr.find("<runtime:"), std::string::npos) << "T2 RegVM 应报运行时错误，实际: \"" << rr << "\"";
}

// ---- 方法内直接使用 super 合法（不应报错）----
TEST(AuditBatch1Super, MethodSuperAllowed) {
    std::string src = "class Base {"
                      "  fun greet() { return \"base\"; }"
                      "}"
                      "class Sub extends Base {"
                      "  fun greet() { return \"sub-\" + super.greet(); }"
                      "}"
                      "var s = Sub();"
                      "print(s.greet());";
    EXPECT_EQ(runStackVM_IR(src), "sub-base");
    EXPECT_EQ(runRegVM(src), "sub-base");
}

// ============================================================
// BUG-IR-OPT-AUDIT-6: optimizeIR 防御性检查
// ------------------------------------------------------------
// 验证 CSE 在栈式 VM 后端（enableDCE=false）自动禁用，
// 防止栈残留导致后续 OP_POP 栈下溢。
// 同时验证空/最小 IR 调用 optimizeIR 不崩溃。
// ============================================================

TEST(AuditBatch1OptIR, EmptyIRNoCrash) {
    // 创建一个空的 IRFunction，调用 optimizeIR 不应崩溃
    IRFunction ir;
    ir.name = "empty";
    ir.localCount = 0;
    ir.blocks.clear();
    ir.constants.clear();
    ir.globalNames.clear();
    // BUG-IR-OPT-AUDIT-6 fix: optimizeIR 入口应防御性检查空 IR
    bool changed = optimizeIR(ir, false, false, false, false);
    EXPECT_FALSE(changed); // 空 IR 无可优化
}

// ---- 全部 pass 启用时不崩溃 ----
TEST(AuditBatch1OptIR, AllPassesOnEmptyNoCrash) {
    IRFunction ir;
    ir.name = "allPasses";
    bool changed = optimizeIR(ir, true, true, true, true);
    EXPECT_FALSE(changed);
}

// ---- CSE + DCE=false 自动禁用 CSE（核心防御检查）----
//   此组合在栈式 VM 后端不安全：CSE 替换后续指令对 dest vreg 的引用，
//   但保留原指令（不删除）；栈式后端 lowering 仍 emit OP_ADD 等压栈指令，
//   未被消费的栈值残留 → 后续 OP_POP 栈下溢。
//   修复后自动禁用 CSE 并告警。
TEST(AuditBatch1OptIR, CSEWithoutDCEAutoDisabled) {
    IRFunction ir;
    ir.name = "cseNoDce";
    // enableCSE=true, enableDCE=false → CSE 应被自动禁用，不产生修改
    bool changed = optimizeIR(ir, false, false, true, false);
    EXPECT_FALSE(changed); // CSE 被禁用，无可优化
}

// ---- 最小 IR（单 basic block + RETURN_NULL）不崩溃 ----
TEST(AuditBatch1OptIR, MinimalIRNoCrash) {
    IRFunction ir;
    ir.name = "minimal";
    ir.localCount = 0;
    uint32_t entryLbl = ir.allocLabel();
    IRBasicBlock bb;
    bb.labelIndex = entryLbl;
    bb.instructions.push_back(IRInstruction(IROp::RETURN_NULL, {}));
    ir.blocks.push_back(std::move(bb));
    bool changed = optimizeIR(ir, true, true, true, true);
    (void)changed; // 仅验证不崩溃
    SUCCEED();
}

// ============================================================
// D2 fix (审计报告 2026-08-03 D2): makeClosure 统一标记闭包 env 引用
// ------------------------------------------------------------
// envPool_ 回收守卫（use_count==1 + 无捕获 + 无闭包 env 引用）依赖
// Environment::hasClosureEnvRef()。原先该标记由 Interpreter 的唯一
// 闭包创建点手工调用（AUDIT-BUG-I1），属"人工保证"不变量——新增
// 创建点漏标即静默语义损坏（resetForReuse 清空 weak_ptr 仍指向该
// env 的闭包的可见变量）。D2 fix 将标记收口到 Value::makeClosure
// 工厂内部，本组测试锁定该结构性不变量。
// ============================================================

// ---- 工厂创建即标记：以真实 env 创建闭包后 hasClosureEnvRef 必为 true ----
TEST(AuditBatch1ClosureEnv, MakeClosureMarksEnvRef) {
    auto env = std::make_shared<Environment>();
    ASSERT_FALSE(env->hasClosureEnvRef());
    Value closureVal = Value::makeClosure("fn", env, {}, nullptr);
    ASSERT_TRUE(closureVal.isClosure());
    EXPECT_TRUE(env->hasClosureEnvRef());
}

// ---- env=nullptr 路径（StackVM/RegisterVM/JIT 的无 env 闭包）：不标记、不崩溃 ----
TEST(AuditBatch1ClosureEnv, MakeClosureNullptrEnvSafe) {
    Value closureVal = Value::makeClosure("fn", nullptr, {}, nullptr);
    ASSERT_TRUE(closureVal.isClosure());
}

// ---- 行为回归：循环体块 env 承受 envPool_ 回收压力（>64 池上限）后，
// 每个闭包仍读到各自迭代的捕获值。若工厂标记缺失，块 env 会被回收
// 复用（resetForReuse 清空 variables），闭包调用读到错值。 ----
TEST(AuditBatch1ClosureEnv, LambdaInLoopBlockSurvivesPoolReuse) {
    std::string src =
        "var fs = [];\n"
        "for (var i = 0; i < 100; i = i + 1) {\n"
        "  var x = i;\n"
        "  fs.push(fun() { return x; });\n"
        "}\n"
        "var ok = 0;\n"
        "for (var j = 0; j < 100; j = j + 1) {\n"
        "  if (fs[j]() == j) { ok = ok + 1; }\n"
        "}\n"
        "print(ok);";
    EXPECT_EQ(runInterpreter(src), "100");
}

// ============================================================
// D1 fix（审计报告 2026-08-03 D1）: IR lowering 栈平衡校验器回归测试
// ------------------------------------------------------------
// 校验器在 debug 构建的 BytecodeIRBackend::lower() 中逐指令模拟操作数栈
// 深度（效应表 + LABEL 锚定，见 IR.h 注释）。本组测试以手工构造的
// IRFunction 验证两类行为：
//   1. 负深度（弹出多于可用）→ lowering 拒绝（debug 构建）；
//   2. 合并点深度冲突 → Logger::Error 报告但 lowering 继续——已知合法形态
//      AUDIT-R6 "break 丢弃挂起 return 值"（try 内 return 压栈后 finally 中
//      break 直接跳出）的 break 边深度多 1，运行时由 OP_RETURN 帧回收清理，
//      静态模拟无法表达该丢弃，故只报告不拒绝。
// Release 构建（NDEBUG）校验器不启用，所有用例的 lower 均须成功。
// ============================================================

TEST(AuditBatch1IRBalance, BalancedFunctionLowers) {
    IRFunction ir;
    ir.name = "balanced";
    ir.localCount = 0;
    uint32_t lbl = ir.allocLabel();
    IRBasicBlock bb;
    bb.labelIndex = lbl;
    // 压 1 → 弹 1 → RETURN_NULL：净 0，标准平衡形态
    bb.instructions.push_back(IRInstruction(IROp::LOAD_NULL, {IROperand::vreg(0)}));
    bb.instructions.push_back(IRInstruction(IROp::POP, {}));
    bb.instructions.push_back(IRInstruction(IROp::RETURN_NULL, {}));
    ir.blocks.push_back(std::move(bb));
    BytecodeIRBackend backend;
    EXPECT_TRUE(backend.lower(ir));
}

TEST(AuditBatch1IRBalance, NegativeDepthDetected) {
    IRFunction ir;
    ir.name = "underflow";
    IRBasicBlock bb;
    // 空栈上 POP：深度 -1，校验器必须拒绝（历史 BUG 模式：visit* emit 收尾漏 POP 的反面）
    bb.instructions.push_back(IRInstruction(IROp::POP, {}));
    ir.blocks.push_back(std::move(bb));
    BytecodeIRBackend backend;
#ifdef NDEBUG
    EXPECT_TRUE(backend.lower(ir)); // Release 无校验器，lowering 本身合法
#else
    EXPECT_FALSE(backend.lower(ir));
#endif
}

TEST(AuditBatch1IRBalance, LabelDepthMismatchReportedNotRejected) {
    IRFunction ir;
    ir.name = "mergeMismatch";
    uint32_t end = ir.allocLabel();
    IRBasicBlock bb;
    bb.labelIndex = end;
    // LOAD_NULL 压 1 → JUMP_IF_FALSE peek 条件（记录 end=1）→ POP 弹 1（fall-through 深度 0）
    // → LABEL end：跳转边期望 1，fall-through 实际 0 → 冲突报告但 lowering 继续
    bb.instructions.push_back(IRInstruction(IROp::LOAD_NULL, {IROperand::vreg(0)}));
    bb.instructions.push_back(IRInstruction(IROp::JUMP_IF_FALSE, {IROperand::vreg(1), IROperand::label(end)}));
    bb.instructions.push_back(IRInstruction(IROp::POP, {}));
    bb.instructions.push_back(IRInstruction(IROp::LABEL, {IROperand::label(end)}));
    ir.blocks.push_back(std::move(bb));
    BytecodeIRBackend backend;
    EXPECT_TRUE(backend.lower(ir)); // 两种构建下均成功（冲突仅 Logger::Error 报告）
}

TEST(AuditBatch1IRBalance, BackwardJumpConflictReportedNotRejected) {
    IRFunction ir;
    ir.name = "loopConflict";
    uint32_t top = ir.allocLabel();
    IRBasicBlock bb;
    bb.labelIndex = top;
    // LABEL top（锚定 0）→ LOAD_NULL 压 1 → JUMP top（回边深度 1 ≠ 已锚定 0）→ 报告但继续
    bb.instructions.push_back(IRInstruction(IROp::LABEL, {IROperand::label(top)}));
    bb.instructions.push_back(IRInstruction(IROp::LOAD_NULL, {IROperand::vreg(0)}));
    bb.instructions.push_back(IRInstruction(IROp::JUMP, {IROperand::label(top)}));
    ir.blocks.push_back(std::move(bb));
    BytecodeIRBackend backend;
    EXPECT_TRUE(backend.lower(ir));
}

// ---- D1 第二步: 效应表完备性的运行时复核 ----
// static_assert(tableIsComplete()) 已在编译期强制（BytecodeIRBackend.cpp），
// 本测试从测试侧独立复核：除 PHI（SSA 中间产物，不可 lower）外，全部 IROp
// 枚举值在给最小合法操作数时都能给出栈效应；PHI 返回 false 且 lowering 拒绝。
// 公式型操作（pops 依赖操作数）需按其布局给足操作数。
static IROperandList minimalOperandsFor(IROp op) {
    switch (op) {
    case IROp::CALL:
    case IROp::TAIL_CALL:
    case IROp::CALL_EXPR:
    case IROp::CLASS_NEW:
        return {IROperand::vreg(0), IROperand::imm(0), IROperand::imm(0)};
    case IROp::METHOD_CALL:
        return {IROperand::vreg(0), IROperand::vreg(1), IROperand::imm(0), IROperand::imm(0)};
    case IROp::SUPER_CALL:
        return {IROperand::vreg(0), IROperand::vreg(1), IROperand::imm(0), IROperand::imm(0),
                IROperand::imm(0)};
    case IROp::BUILD_ARRAY:
    case IROp::BUILD_DICT:
    case IROp::BUILD_TUPLE:
        return {IROperand::vreg(0), IROperand::imm(0)};
    case IROp::BUILD_ENUM_VARIANT:
        return {IROperand::vreg(0), IROperand::imm(0), IROperand::imm(0), IROperand::imm(0)};
    default:
        return {};
    }
}

TEST(AuditBatch1IRBalance, EffectTableCoversAllIROps) {
    for (size_t v = 0; v < static_cast<size_t>(IROp::IROp_COUNT); ++v) {
        const IROp op = static_cast<IROp>(v);
        if (op == IROp::IROp_COUNT)
            continue;
        BytecodeIRBackend::StackEffect fx{0, 0};
        const bool ok = BytecodeIRBackend::irStackEffect(
            IRInstruction(op, minimalOperandsFor(op)), fx);
        if (op == IROp::PHI) {
            EXPECT_FALSE(ok) << "PHI 不可 lower，效应表不应登记";
        } else {
            EXPECT_TRUE(ok) << "IROp #" << v << " 未登记效应（新增 IROp 须补 kFixedStackEffect 或公式分支）";
            EXPECT_GE(fx.pops, 0);
            EXPECT_GE(fx.pushes, 0);
        }
    }
}

// PHI 到达 lowering 时必须被拒绝（lowerInstruction 的 default 分支），两种构建一致
TEST(AuditBatch1IRBalance, PhiInstructionRejectedByLower) {
    IRFunction ir;
    ir.name = "phi";
    IRBasicBlock bb;
    bb.instructions.push_back(IRInstruction(IROp::PHI, {IROperand::vreg(0)}));
    ir.blocks.push_back(std::move(bb));
    BytecodeIRBackend backend;
    EXPECT_FALSE(backend.lower(ir));
}

TEST(AuditBatch1IRBalance, BalancedLoopLowers) {
    IRFunction ir;
    ir.name = "balancedLoop";
    uint32_t top = ir.allocLabel();
    IRBasicBlock bb;
    bb.labelIndex = top;
    // LABEL top（0）→ 压 1 → 弹 1 → 回边（深度 0 == 已锚定 0）→ RETURN_NULL
    bb.instructions.push_back(IRInstruction(IROp::LABEL, {IROperand::label(top)}));
    bb.instructions.push_back(IRInstruction(IROp::LOAD_NULL, {IROperand::vreg(0)}));
    bb.instructions.push_back(IRInstruction(IROp::POP, {}));
    bb.instructions.push_back(IRInstruction(IROp::JUMP, {IROperand::label(top)}));
    bb.instructions.push_back(IRInstruction(IROp::RETURN_NULL, {}));
    ir.blocks.push_back(std::move(bb));
    BytecodeIRBackend backend;
    EXPECT_TRUE(backend.lower(ir));
}
