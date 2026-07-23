// ============================================================
// TestFrameSwitching.cpp — R121 调用栈帧切换回归测试
// ------------------------------------------------------------
// 测试范围：
//   1. Interpreter::getCallStackSnapshot() 在递归调用栈中返回多帧
//   2. 各帧 env->snapshotLocalVariables() 返回该帧独立的局部变量
//   3. 越界访问安全返回空（getCallStackSnapshot 不崩溃）
//   4. CallStackLibrary 静态场景库完整性（帧切换教学场景已收录）
//
// 依赖约束：
//   - IdeController.cpp / DebugCoordinator.cpp 不在 minilang_core 中，
//     无法直接测试 IdeController::setSelectedFrame / getSelectedFrameLocals。
//     上层 API 通过代码审查 + IDE 手动测试验证（与 TestRunToCursor.cpp 中
//     VmStepper 注释一致）。
//   - 本测试聚焦底层机制：Interpreter::getCallStackSnapshot() +
//     Environment::snapshotLocalVariables() 在递归暂停状态下能正确返回
//     各帧独立的局部变量，这是 R121 帧切换机制的底层数据源。
//
// 测试框架：GoogleTest
// ============================================================

#include <gtest/gtest.h>

#include "ast/ASTNode.h"
#include "debug/DebugController.h"
#include "debug/DebugTypes.h"
#include "gui/CallStackPanel.h"
#include "interpreter/CallFrame.h"
#include "interpreter/Environment.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

// ============================================================
// 辅助函数
// ============================================================

static std::unique_ptr<Block> parseSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    return parser.parse(tokens);
}

// ============================================================
// 测试套件 1：Interpreter 调用栈快照 — 递归调用栈帧独立性
// ============================================================

/// 验证递归调用栈中各帧的局部变量独立，是 R121 帧切换机制的底层数据源。
/// 测试场景：fib(2) → fib(1)，断点设在 fib 函数体 return n 处，
/// 暂停时调用栈应为 [main, fib(2), fib(1)]，各帧 n 值不同。
TEST(FrameSwitchingInterpreter, RecursiveCallStack_HasDistinctFrameLocals) {
    const std::string source = "fun fib(n) {\n"                       // line 1
                               "    if (n <= 1) {\n"                  // line 2
                               "        return n;\n"                  // line 3
                               "    }\n"                              // line 4
                               "    return fib(n - 1) + fib(n - 2);\n" // line 5
                               "}\n"                                  // line 6
                               "fib(2);\n";                           // line 7
    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setBreakpoints({3}); // 在 return n; 处设断点

    std::atomic<int> pauseCount{0};
    std::atomic<int> lastPausedLine{-1};
    QObject::connect(dbg.get(), &DebugController::pausedAt, dbg.get(),
                     [&](int line) {
                         lastPausedLine.store(line);
                         pauseCount.fetch_add(1);
                     },
                     Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // 等待首次暂停（fib(1) 命中 line 3）
    for (int i = 0; i < 200 && pauseCount.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(pauseCount.load(), 1) << "应在 line 3 暂停";
    ASSERT_EQ(lastPausedLine.load(), 3);

    // 获取调用栈快照：[main, fib(2), fib(1)]
    auto stack = interp.getCallStackSnapshot();
    EXPECT_GE(stack.size(), 3u) << "至少应有 main + fib(2) + fib(1) 三帧";

    if (stack.size() >= 3) {
        // 第 0 帧：main
        EXPECT_EQ(stack[0].depth, 0);
        // 第 1 帧：fib(2)
        EXPECT_EQ(stack[1].depth, 1);
        ASSERT_NE(stack[1].env, nullptr);
        auto fib2Locals = stack[1].env->snapshotLocalVariables();
        ASSERT_TRUE(fib2Locals.count("n")) << "fib(2) 帧应有局部变量 n";
        EXPECT_EQ(fib2Locals["n"].intVal(), 2) << "fib(2) 帧的 n 应为 2";

        // 第 2 帧：fib(1)（栈顶，正在执行 return n）
        EXPECT_EQ(stack[2].depth, 2);
        ASSERT_NE(stack[2].env, nullptr);
        auto fib1Locals = stack[2].env->snapshotLocalVariables();
        ASSERT_TRUE(fib1Locals.count("n")) << "fib(1) 帧应有局部变量 n";
        EXPECT_EQ(fib1Locals["n"].intVal(), 1) << "fib(1) 帧的 n 应为 1";
    }

    // 清理
    dbg->stop();
    execThread.join();
}

/// 验证空调用栈快照安全返回（无递归调用时 getCallStackSnapshot 不崩溃）。
TEST(FrameSwitchingInterpreter, EmptyCallStackSnapshot_ReturnsEmptyVector) {
    Interpreter interp;
    auto stack = interp.getCallStackSnapshot();
    EXPECT_TRUE(stack.empty()) << "未执行的 Interpreter 调用栈应为空";
}

/// 验证调用栈快照的 depth 字段从 0 递增。
TEST(FrameSwitchingInterpreter, CallStackSnapshot_DepthFieldIsSequential) {
    const std::string source = "fun foo() {\n"  // line 1
                               "    bar();\n"   // line 2
                               "}\n"            // line 3
                               "fun bar() {\n"  // line 4
                               "    return 1;\n" // line 5
                               "}\n"            // line 6
                               "foo();\n";      // line 7
    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setBreakpoints({5}); // bar 函数体内 return 1 处

    std::atomic<int> pauseCount{0};
    QObject::connect(dbg.get(), &DebugController::pausedAt, dbg.get(),
                     [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    for (int i = 0; i < 200 && pauseCount.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(pauseCount.load(), 1);

    auto stack = interp.getCallStackSnapshot();
    EXPECT_GE(stack.size(), 3u) << "应有 main + foo + bar 三帧";

    // depth 字段应从 0 递增
    for (size_t i = 0; i < stack.size(); ++i) {
        EXPECT_EQ(stack[i].depth, static_cast<int>(i)) << "帧 " << i << " 的 depth 应为 " << i;
    }

    // 函数名序列应为 <main>, foo, bar
    if (stack.size() >= 3) {
        EXPECT_FALSE(stack[0].functionName.empty());
        EXPECT_EQ(stack[1].functionName, "foo");
        EXPECT_EQ(stack[2].functionName, "bar");
    }

    dbg->stop();
    execThread.join();
}

// ============================================================
// 测试套件 2：CallStackLibrary — 帧切换教学场景完整性
// ============================================================

/// 验证 CallStackLibrary 中包含递归场景（最典型的帧切换演示用例）。
TEST(FrameSwitchingCallStackLibrary, RecursionScenarioExists) {
    const auto& scenarios = CallStackLibrary::scenarios();
    bool hasRecursion = false;
    for (const auto& s : scenarios) {
        if (s.id == "recursion") {
            hasRecursion = true;
            EXPECT_FALSE(s.expectedFrames.empty()) << "recursion 场景应包含期望栈帧序列";
            break;
        }
    }
    EXPECT_TRUE(hasRecursion) << "CallStackLibrary 应包含 recursion 场景";
}

/// 验证 CallStackLibrary 各场景的 expectedFrames 字段非空（教学期望明确）。
TEST(FrameSwitchingCallStackLibrary, AllScenariosHaveExpectedFrames) {
    const auto& scenarios = CallStackLibrary::scenarios();
    EXPECT_FALSE(scenarios.empty());
    for (const auto& s : scenarios) {
        EXPECT_FALSE(s.expectedFrames.empty()) << "场景 " << s.id << " 的 expectedFrames 不应为空";
    }
}

/// 验证 CallStackLibrary 场景 ID 唯一。
TEST(FrameSwitchingCallStackLibrary, ScenarioIdsAreUnique) {
    const auto& scenarios = CallStackLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) {
        auto [_, inserted] = ids.insert(s.id);
        EXPECT_TRUE(inserted) << "场景 id 重复: " << s.id;
    }
    EXPECT_EQ(ids.size(), scenarios.size());
}

// ============================================================
// 测试套件 3：CallStackEntry 结构完整性
// ============================================================

/// 验证 CallStackEntry 结构包含帧切换所需的全部字段（functionName/line/depth/locals）。
TEST(FrameSwitchingCallStackEntry, StructHasRequiredFields) {
    CallStackEntry entry;
    entry.functionName = "test-fn";
    entry.line = 42;
    entry.depth = 3;
    entry.locals.emplace_back("x", Value(123));

    EXPECT_EQ(entry.functionName, "test-fn");
    EXPECT_EQ(entry.line, 42);
    EXPECT_EQ(entry.depth, 3);
    ASSERT_EQ(entry.locals.size(), 1u);
    EXPECT_EQ(entry.locals[0].first, "x");
    EXPECT_EQ(entry.locals[0].second.intVal(), 123);
}

/// 验证 CallStackEntry 默认构造字段为空/零值。
TEST(FrameSwitchingCallStackEntry, DefaultConstructor_ZeroesFields) {
    CallStackEntry entry;
    EXPECT_TRUE(entry.functionName.empty());
    EXPECT_EQ(entry.line, 0);
    EXPECT_EQ(entry.depth, 0);
    EXPECT_TRUE(entry.locals.empty());
}
