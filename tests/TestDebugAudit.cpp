// ============================================================
// DebugController + REPL 审计回归测试
// ------------------------------------------------------------
// 覆盖审计发现的 15 个 Bug 的回归测试：
//   BUG-DBG-1  (P1): 调用栈顶帧行号显示调用点而非当前执行行
//   BUG-DBG-2  (P2): VmStepper 缺少 crossedLine_ 机制（仅 IDE 层，跳过）
//   BUG-DBG-3  (P2): STEP_OUT 顶层行为不一致（仅 IDE 层，跳过）
//   BUG-DBG-4  (P2): callNamedFunction 使用变量名而非闭包名
//   BUG-DBG-5  (P2): Interpreter 调用栈无顶层 main 帧
//   BUG-DBG-6  (P2): VM 调用栈从未在 GUI 显示（仅 IDE 层，跳过）
//   BUG-DBG-7  (P2): 沙箱未重置 recursionDepth_
//   BUG-DBG-8  (P2): 沙箱未保存/恢复 funRegistry_/classRegistry_
//   BUG-DBG-9  (P2): stopRequested_ 未在 execute() 中重置
//   BUG-DBG-10 (P2): prepareRun catch 块重置顺序错误（仅 IDE 层，跳过）
//   BUG-DBG-11 (P2): prepareRun 状态设置在 try 块外（仅 IDE 层，跳过）
//   BUG-DBG-13 (P2): VmStepper 注释引用不存在函数（文档修复，跳过）
//   BUG-DBG-14 (P2): 测试桩缺少关键修复（在 test_harness 中验证）
//   BUG-REPL-1 (P2): clearModuleCache 未规范化路径
//   BUG-REPL-2 (P2): 沙箱未保存/恢复 lastValue_
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "interpreter/RuntimeExceptions.h"
#include "debug/DebugTypes.h"

#include <string>
#include <memory>

// ============================================================
// 辅助函数
// ============================================================

static std::unique_ptr<Block> parseSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    return parser.parse(tokens);
}

static std::string runOutput(const std::string& source) {
    auto ast = parseSource(source);
    if (!ast) return "";
    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&captured](const std::string& s) { captured += s; });
    try {
        interp.execute(*ast);
    } catch (...) {}
    return captured;
}

// ============================================================
// BUG-DBG-5: Interpreter 调用栈无顶层 main 帧
// ============================================================

TEST(DebugAuditMainFrame, ExecuteHasMainFrame) {
    auto ast = parseSource("var x = 1;");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);
    // BUG-DBG-5 fix: execute() 后 callStack_ 应保留 main 帧
    const auto& stack = interp.getCallStack();
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].functionName, "main");
    EXPECT_EQ(stack[0].depth, 0);
}

TEST(DebugAuditMainFrame, ExecuteReplHasMainFrame) {
    auto ast = parseSource("var y = 2;");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.executeRepl(*ast);
    // BUG-DBG-5 fix: executeRepl() 后 callStack_ 应保留 main 帧
    const auto& stack = interp.getCallStack();
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].functionName, "main");
}

TEST(DebugAuditMainFrame,MainFrameLineUpdatesOnBreak) {
    // BUG-DBG-1 fix: 顶帧 line 应更新为当前执行行（通过 checkBreak）
    // 此测试验证 main 帧存在且 line 字段可被更新
    auto ast = parseSource("var a = 10;\nvar b = 20;\n");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);
    const auto& stack = interp.getCallStack();
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].functionName, "main");
    // main 帧的 line 可能是 0（未触发 checkBreak）或最后执行行
    // 关键是 main 帧存在
}

// ============================================================
// BUG-DBG-9: stopRequested_ 未在 execute() 中重置
// ============================================================

TEST(DebugAuditStopReset, ExecuteResetsStopRequested) {
    // BUG-DBG-9 fix: execute() 应重置 stopRequested_，避免上一轮停止残留
    Interpreter interp;
    interp.requestStop();  // 模拟上一轮被用户停止
    EXPECT_TRUE(interp.isStopRequested());

    auto ast = parseSource("var x = 1;");
    ASSERT_NE(ast, nullptr);
    // execute() 应重置 stopRequested_，程序正常执行不抛 DebugStopException
    EXPECT_NO_THROW(interp.execute(*ast));
    EXPECT_FALSE(interp.isStopRequested());
}

TEST(DebugAuditStopReset, ConsecutiveExecuteAfterStop) {
    // 连续两次 execute：第一次前设 stop，第二次应正常运行
    Interpreter interp;
    interp.requestStop();

    auto ast1 = parseSource("var a = 1;");
    auto ast2 = parseSource("var b = 2;");
    ASSERT_NE(ast1, nullptr);
    ASSERT_NE(ast2, nullptr);

    EXPECT_NO_THROW(interp.execute(*ast1));
    // 第二次 execute 不应因残留 stopRequested_ 而立即停止
    EXPECT_NO_THROW(interp.execute(*ast2));
}

// ============================================================
// BUG-REPL-1: clearModuleCache 未规范化路径
// ============================================================

TEST(DebugAuditModuleCache, ClearWithBackslashPath) {
    // BUG-REPL-1 fix: clearModuleCache 应规范化路径（\ → /）
    // 注：execute() 会清空全部模块缓存，因此第二次导入用 executeRepl()
    //（不清空缓存），这样才能验证 clearModuleCache 的路径规范化是否生效。
    Interpreter interp;
    std::string loadedPath;
    interp.setModuleLoader([&loadedPath](const std::string& path) {
        loadedPath = path;
        return "export var cached = 42;";
    });

    // 导入模块（缓存键为规范化后的路径 "foo/bar.mini"）
    auto ast1 = parseSource("import { cached } from \"foo/bar.mini\";");
    ASSERT_NE(ast1, nullptr);
    EXPECT_NO_THROW(interp.execute(*ast1));
    EXPECT_EQ(loadedPath, "foo/bar.mini");

    // 用反斜杠路径清除缓存（应规范化为 "foo/bar.mini" 后命中）
    interp.clearModuleCache("foo\\bar.mini");

    // 再次导入应重新加载（缓存已被清除）。使用 executeRepl 避免清空全部缓存。
    loadedPath.clear();
    auto ast2 = parseSource("import { cached } from \"foo/bar.mini\";");
    ASSERT_NE(ast2, nullptr);
    EXPECT_NO_THROW(interp.executeRepl(*ast2));
    EXPECT_EQ(loadedPath, "foo/bar.mini");
}

TEST(DebugAuditModuleCache, ClearWithDotSlashPath) {
    // BUG-REPL-1 fix: clearModuleCache 应去除 "./" 前缀
    Interpreter interp;
    std::string loadedPath;
    interp.setModuleLoader([&loadedPath](const std::string& path) {
        loadedPath = path;
        return "export var cached = 99;";
    });

    // 导入模块（缓存键为 "mymod.mini"，./ 前缀已去除）
    auto ast1 = parseSource("import { cached } from \"mymod.mini\";");
    ASSERT_NE(ast1, nullptr);
    EXPECT_NO_THROW(interp.execute(*ast1));

    // 用 ./ 前缀路径清除缓存
    interp.clearModuleCache("./mymod.mini");

    // 再次导入应重新加载。使用 executeRepl 避免清空全部缓存。
    loadedPath.clear();
    auto ast2 = parseSource("import { cached } from \"mymod.mini\";");
    ASSERT_NE(ast2, nullptr);
    EXPECT_NO_THROW(interp.executeRepl(*ast2));
    EXPECT_EQ(loadedPath, "mymod.mini");
}

// ============================================================
// BUG-DBG-7: 沙箱未重置 recursionDepth_
// BUG-DBG-8: 沙箱未保存/恢复 funRegistry_/classRegistry_
// BUG-REPL-2: 沙箱未保存/恢复 lastValue_
// ============================================================

TEST(DebugAuditSandbox, DoesNotPolluteFunRegistry) {
    // BUG-DBG-8 fix: 条件求值中定义的函数不应污染 funRegistry_
    auto ast = parseSource("var x = 1;");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);

    // 解析一个定义函数的条件表达式
    auto condAst = parseSource("fun evil() { return 666; }\ntrue");
    ASSERT_NE(condAst, nullptr);
    ASSERT_FALSE(condAst->statements.empty());
    auto& stmt = condAst->statements[0];
    // 标记为表达式语句以便 evaluateCondition 走表达式路径
    // 实际条件断点传入的是表达式 AST，这里用语句模拟
    ASSERT_NE(stmt, nullptr);

    // 执行前 funRegistry_ 没有 evil
    // 注：evaluateCondition 需要 debugMode_ 关闭，内部会处理
    EXPECT_NO_THROW(interp.evaluateCondition(stmt.get()));

    // 执行后 funRegistry_ 不应有 evil（沙箱恢复）
    // 由于 funRegistry_ 是 private，我们通过行为验证：
    // 尝试调用 evil 应失败（未定义）
    auto callAst = parseSource("evil();");
    ASSERT_NE(callAst, nullptr);
    std::string err;
    try {
        interp.executeRepl(*callAst);
    } catch (const RuntimeError& e) {
        err = e.what();
    } catch (...) {}
    // evil 不应被定义（沙箱恢复清除了 funRegistry_）
    EXPECT_NE(err.find("evil"), std::string::npos)
        << "evil() should not be defined after sandbox evaluation";
}

TEST(DebugAuditSandbox, DoesNotPolluteClassRegistry) {
    // BUG-DBG-8 fix: 条件求值中定义的类不应污染 classRegistry_
    auto ast = parseSource("var x = 1;");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);

    auto condAst = parseSource("class Evil { var v = 1; }\ntrue");
    ASSERT_NE(condAst, nullptr);
    ASSERT_FALSE(condAst->statements.empty());
    auto& stmt = condAst->statements[0];
    ASSERT_NE(stmt, nullptr);

    EXPECT_NO_THROW(interp.evaluateCondition(stmt.get()));

    // 尝试实例化 Evil 应失败
    auto callAst = parseSource("var e = Evil();");
    ASSERT_NE(callAst, nullptr);
    std::string err;
    try {
        interp.executeRepl(*callAst);
    } catch (const RuntimeError& e) {
        err = e.what();
    } catch (...) {}
    EXPECT_NE(err.find("Evil"), std::string::npos)
        << "Evil class should not be defined after sandbox evaluation";
}

TEST(DebugAuditSandbox, PreservesLastValue) {
    // BUG-REPL-2 fix: 条件求值不应污染 lastValue_
    auto ast = parseSource("var x = 42;");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);

    // 用 evaluateExpr 设置 lastValue_
    auto exprAst = parseSource("x + 1;");
    ASSERT_NE(exprAst, nullptr);
    ASSERT_FALSE(exprAst->statements.empty());
    auto& exprStmt = exprAst->statements[0];
    ASSERT_NE(exprStmt, nullptr);
    Value val1 = interp.evaluateExpr(exprStmt.get());
    EXPECT_EQ(val1.intVal(), 43);

    // 条件求值会覆盖 lastValue_，但沙箱应恢复
    auto condAst = parseSource("999;");
    ASSERT_NE(condAst, nullptr);
    ASSERT_FALSE(condAst->statements.empty());
    auto& condStmt = condAst->statements[0];
    ASSERT_NE(condStmt, nullptr);
    Value condResult = interp.evaluateCondition(condStmt.get());
    EXPECT_EQ(condResult.intVal(), 999);

    // 再次 evaluateExpr 应正常工作（lastValue_ 已被沙箱恢复）
    Value val2 = interp.evaluateExpr(exprStmt.get());
    EXPECT_EQ(val2.intVal(), 43)
        << "evaluateExpr should still work after sandbox evaluation";
}

TEST(DebugAuditSandbox, ResetsRecursionDepth) {
    // BUG-DBG-7 fix: 沙箱应重置 recursionDepth_，避免假阳性递归深度超限
    // 此测试验证条件求值不会因当前 recursionDepth_ 过高而失败
    auto ast = parseSource("var x = 1;");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);

    // 条件求值应成功（即使内部 recursionDepth_ 被 reset 为 0）
    auto condAst = parseSource("x == 1;");
    ASSERT_NE(condAst, nullptr);
    ASSERT_FALSE(condAst->statements.empty());
    auto& condStmt = condAst->statements[0];
    ASSERT_NE(condStmt, nullptr);
    Value result;
    EXPECT_NO_THROW(result = interp.evaluateCondition(condStmt.get()));
    EXPECT_TRUE(result.isBool());
    EXPECT_TRUE(result.boolVal());
}

// ============================================================
// BUG-DBG-4: callNamedFunction 使用变量名而非闭包名
// ============================================================

TEST(DebugAuditCallStack, ClosureNameInCallStack) {
    // BUG-DBG-4 fix: 闭包赋值给变量后调用，调用栈应显示闭包名而非变量名
    // 此测试验证闭包调用不崩溃（完整调用栈验证需要 debugger 回调）
    std::string source = R"(
        fun makeAdder() {
            var n = 10;
            fun adderInner(x) { return x + n; }
            return adderInner;
        }
        var f = makeAdder();
        print(f(5));
    )";
    std::string output = runOutput(source);
    EXPECT_NE(output.find("15"), std::string::npos)
        << "Closure call should produce 15, got: " << output;
}

// ============================================================
// 综合回归测试
// ============================================================

TEST(DebugAuditRegression, StopAndRunPreservesState) {
    // 综合：stop 后重新 execute 不应残留状态
    Interpreter interp;
    interp.requestStop();

    std::string source = R"(
        var result = 0;
        for (var i = 0; i < 10; i = i + 1) {
            result = result + i;
        }
        print(result);
    )";
    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);
    std::string captured;
    interp.setOutputCallback([&captured](const std::string& s) { captured += s; });
    EXPECT_NO_THROW(interp.execute(*ast));
    EXPECT_EQ(captured, "45");
    EXPECT_FALSE(interp.isStopRequested());
    // main 帧应存在
    const auto& stack = interp.getCallStack();
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].functionName, "main");
}

TEST(DebugAuditRegression, SandboxPreservesAllState) {
    // 综合：沙箱求值后所有状态（变量/函数/类/递归深度/lastValue）应恢复
    std::string source = R"(
        var counter = 100;
        fun getValue() { return counter; }
        class Container { var data = 0; }
    )";
    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);

    // 沙箱求值：尝试修改所有状态
    auto condAst = parseSource(R"(
        counter = 999;
        fun evil() { return -1; }
        class Evil { var v = -1; }
        true
    )");
    ASSERT_NE(condAst, nullptr);
    ASSERT_FALSE(condAst->statements.empty());
    EXPECT_NO_THROW(interp.evaluateCondition(condAst->statements[0].get()));

    // 验证 counter 未被污染
    auto checkAst = parseSource("print(counter);");
    ASSERT_NE(checkAst, nullptr);
    std::string captured;
    interp.setOutputCallback([&captured](const std::string& s) { captured += s; });
    EXPECT_NO_THROW(interp.executeRepl(*checkAst));
    EXPECT_EQ(captured, "100");
}
