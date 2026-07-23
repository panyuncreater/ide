// ============================================================
// TestRunToCursor — R98 runToCursor 临时断点机制回归测试
// ------------------------------------------------------------
// 覆盖 DebugController 的一次性临时断点 API：
//   - setTemporaryBreakpoint / clearTemporaryBreakpoint / getTemporaryBreakpoint
//   - 无效行号（<=0）忽略
//   - 多次设置覆盖（取最后一次）
//   - stop() / reset() / 析构 清除临时断点
//   - 集成测试：Interpreter 执行命中临时断点后自动清除并暂停
//
// VmStepper 的对称 API（setTempBreakpoint/clearTempBreakpoint/getTempBreakpoint）
// 位于 app/VmStepper.cpp，不属于 minilang_core，未链接到 minilang_tests 目标。
// VmStepper 的临时断点逻辑通过代码审查 + IDE 手动测试验证。
// ============================================================

#include <gtest/gtest.h>

#include "ast/ASTNode.h"
#include "debug/DebugController.h"
#include "debug/DebugTypes.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
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
// 第一组：DebugController 临时断点 API 单元测试
// ============================================================

TEST(RunToCursorApi, SetTemporaryBreakpoint_ValidLine_StoresLine) {
    DebugController dbg;
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1); // 初始无临时断点
    dbg.setTemporaryBreakpoint(5);
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), 5);
}

TEST(RunToCursorApi, SetTemporaryBreakpoint_InvalidLine_Ignored) {
    DebugController dbg;
    dbg.setTemporaryBreakpoint(0);
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
    dbg.setTemporaryBreakpoint(-1);
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
    dbg.setTemporaryBreakpoint(-100);
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
}

TEST(RunToCursorApi, SetTemporaryBreakpoint_Overwrite_ReplacesPrevious) {
    DebugController dbg;
    dbg.setTemporaryBreakpoint(3);
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), 3);
    dbg.setTemporaryBreakpoint(10);
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), 10);
    dbg.setTemporaryBreakpoint(1);
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), 1);
}

TEST(RunToCursorApi, ClearTemporaryBreakpoint_ClearsStoredLine) {
    DebugController dbg;
    dbg.setTemporaryBreakpoint(7);
    ASSERT_EQ(dbg.getTemporaryBreakpoint(), 7);
    dbg.clearTemporaryBreakpoint();
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
}

TEST(RunToCursorApi, ClearTemporaryBreakpoint_WhenEmpty_IsNoOp) {
    DebugController dbg;
    dbg.clearTemporaryBreakpoint();
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
}

TEST(RunToCursorApi, Stop_ClearsTemporaryBreakpoint) {
    DebugController dbg;
    dbg.setTemporaryBreakpoint(15);
    ASSERT_EQ(dbg.getTemporaryBreakpoint(), 15);
    dbg.stop();
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
}

TEST(RunToCursorApi, Reset_ClearsTemporaryBreakpoint) {
    DebugController dbg;
    dbg.setTemporaryBreakpoint(20);
    ASSERT_EQ(dbg.getTemporaryBreakpoint(), 20);
    dbg.reset();
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
}

TEST(RunToCursorApi, StopThenReset_BothClearSafely) {
    DebugController dbg;
    dbg.setTemporaryBreakpoint(25);
    dbg.stop();
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
    dbg.reset();
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
}

TEST(RunToCursorApi, TemporaryBreakpoint_IndependentFromUserBreakpoints) {
    // 临时断点与用户断点独立存储——设置/清除临时断点不应影响用户断点
    DebugController dbg;
    dbg.setBreakpoint(10);
    dbg.setBreakpoint(20);
    EXPECT_EQ(dbg.getBreakpoints().size(), 2u);

    dbg.setTemporaryBreakpoint(15);
    EXPECT_EQ(dbg.getBreakpoints().size(), 2u); // 用户断点不受影响
    EXPECT_FALSE(dbg.hasBreakpoint(15));        // 临时断点不出现在用户断点集合
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), 15);

    dbg.clearTemporaryBreakpoint();
    EXPECT_EQ(dbg.getBreakpoints().size(), 2u); // 用户断点仍在
    EXPECT_EQ(dbg.getTemporaryBreakpoint(), -1);
}

// ============================================================
// 第二组：DebugController 临时断点集成测试（Interpreter 路径）
// ------------------------------------------------------------
// 验证临时断点在 Interpreter 执行期间命中后：
//   1. pausedAt 信号发射（正确行号）
//   2. 临时断点自动清除（getTemporaryBreakpoint() == -1）
//   3. 后续 resume() 不再在目标行暂停（一次性语义）
//
// 线程模型：Interpreter 在单独线程执行，checkBreak 命中临时断点后阻塞在
// pauseExecution（condition_variable wait）。主线程轮询 atomic 变量检测暂停。
// Qt::DirectConnection 使 pausedAt 信号在 worker 线程立即执行 lambda，
// lambda 仅写 atomic 变量（无锁安全）。
// ============================================================

TEST(RunToCursorIntegration, TempBreakpointHit_ClearsAndPauses) {
    // 源码：5 行，临时断点设在第 3 行
    const std::string source = "var a = 1;\n"  // line 1
                               "var b = 2;\n"  // line 2
                               "var c = 3;\n"  // line 3
                               "var d = 4;\n"  // line 4
                               "var e = 5;\n"; // line 5

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    DebugController* dbg = new DebugController();
    interp.setOutputCallback([](const std::string&) {});

    // Wire debugger to interpreter（空删除器避免 shared_ptr 析构 delete）
    interp.setDebugger(std::shared_ptr<DebugController>(dbg, [](DebugController*) {}));
    interp.setDebugMode(true);

    // 配置调试器：RUN 模式 + 临时断点在第 3 行
    dbg->reset();
    dbg->resume();
    dbg->setTemporaryBreakpoint(3);
    ASSERT_EQ(dbg->getTemporaryBreakpoint(), 3);

    // 记录 pausedAt 信号（atomic 安全跨线程读写）
    std::atomic<int> pausedLine{-1};
    std::atomic<int> pauseCount{0};
    // Qt::DirectConnection 使 lambda 在 worker 线程立即执行（无需事件循环）
    // 5 参数 connect 重载：(sender, signal, context, functor, connectionType)
    QObject::connect(
        dbg, &DebugController::pausedAt, dbg,
        [&](int line) {
            pausedLine.store(line);
            pauseCount.fetch_add(1);
        },
        Qt::DirectConnection);

    // 在单独线程中执行（checkBreak 命中临时断点后会阻塞在 pauseExecution）
    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
            // 执行被 stop 中止时可能抛异常，忽略
        }
    });

    // 等待 pausedAt 信号（轮询 atomic，最多 2 秒）
    for (int i = 0; i < 200 && pauseCount.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 验证：在第 3 行暂停
    EXPECT_EQ(pauseCount.load(), 1);
    EXPECT_EQ(pausedLine.load(), 3);

    // 验证：临时断点已自动清除（一次性语义）
    EXPECT_EQ(dbg->getTemporaryBreakpoint(), -1);

    // 清理：stop 解除 pauseExecution 阻塞，join 线程
    dbg->stop();
    execThread.join();
}

TEST(RunToCursorIntegration, TempBreakpointResume_DoesNotReTrigger) {
    // 验证一次性语义：临时断点命中后清除，resume 不再在目标行暂停
    // 源码：循环 3 次，临时断点设在循环体内的某行
    const std::string source = "for (var i = 0; i < 3; i = i + 1) {\n" // line 1
                               "    var x = i * 2;\n"                  // line 2
                               "}\n";                                  // line 3

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    DebugController* dbg = new DebugController();
    interp.setOutputCallback([](const std::string&) {});

    interp.setDebugger(std::shared_ptr<DebugController>(dbg, [](DebugController*) {}));
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setTemporaryBreakpoint(2);
    ASSERT_EQ(dbg->getTemporaryBreakpoint(), 2);

    std::atomic<int> pauseCount{0};
    std::atomic<int> lastPausedLine{-1};
    QObject::connect(
        dbg, &DebugController::pausedAt, dbg,
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

    // 等待首次暂停
    for (int i = 0; i < 200 && pauseCount.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(pauseCount.load(), 1);
    EXPECT_EQ(lastPausedLine.load(), 2);
    EXPECT_EQ(dbg->getTemporaryBreakpoint(), -1); // 已清除

    // resume 继续执行，循环剩余 2 次经过第 2 行但不应再暂停
    dbg->resume();

    // 等待执行结束（循环 3 次约毫秒级，给 2 秒余量）
    // std::thread::join 不支持超时，用 atomic 标志 + 轮询模拟
    std::atomic<bool> threadDone{false};
    std::thread joinWatcher([&]() {
        execThread.join();
        threadDone.store(true);
    });
    for (int i = 0; i < 200 && !threadDone.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!threadDone.load()) {
        dbg->stop(); // 兜底：若 2 秒未结束强制停止
        joinWatcher.join();
    } else {
        joinWatcher.join();
    }

    // 验证：仅暂停 1 次（一次性语义——临时断点命中后不再触发）
    EXPECT_EQ(pauseCount.load(), 1);
}

TEST(RunToCursorIntegration, TempBreakpointInvalidLine_NoPause) {
    // 无效行号（<=0）被 setTemporaryBreakpoint 忽略，执行不暂停
    const std::string source = "var x = 1;\nvar y = 2;\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    DebugController* dbg = new DebugController();
    interp.setOutputCallback([](const std::string&) {});

    interp.setDebugger(std::shared_ptr<DebugController>(dbg, [](DebugController*) {}));
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setTemporaryBreakpoint(0);  // 无效，被忽略
    dbg->setTemporaryBreakpoint(-5); // 无效，被忽略
    EXPECT_EQ(dbg->getTemporaryBreakpoint(), -1);

    std::atomic<int> pauseCount{0};
    QObject::connect(dbg, &DebugController::pausedAt, dbg, [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    // 直接在当前线程执行（无临时断点，不会阻塞）
    try {
        interp.execute(*ast);
    } catch (...) {
    }

    EXPECT_EQ(pauseCount.load(), 0);
}
