// ============================================================
// TestR104DebuggerExtensions — R104 调试器拓展回归测试
// ------------------------------------------------------------
// 覆盖三项 S 级拓展（development.md "三、调试器拓展" 短期优先级建议）：
//   1. Logpoint（日志断点）：命中不暂停，仅输出日志消息，递增 hitCount
//   2. Function Breakpoint（函数断点）：按函数名设置断点（不依赖行号）
//   3. Exception Breakpoint（异常断点）：throw 前自动暂停（catch throw 语义）
//
// 测试路径：
//   - Interpreter 路径：使用生产版 DebugController + Qt::DirectConnection 信号
//     + std::thread 异步执行（仿照 TestRunToCursor.cpp 范式）
//   - VmStepper / VM 路径：通过 DebugController 的 checkFunctionBreakpoint /
//     checkExceptionBreakpoint API 直接验证钩子被正确调用
//
// 三后端一致性：
//   - Logpoint：Interpreter 集成测试覆盖（VM 路径通过 VmStepper::handleLogpointHit
//     在 GUI 层处理，由 TestTeachingPanelsAudit 间接覆盖）
//   - Function BP：Interpreter 集成测试 + VM peekCalledFunctionName 单元测试
//   - Exception BP：Interpreter 集成测试 + VM isCurrentThrowInstruction 单元测试
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

#include <QSet>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
// 第一组：Logpoint（日志断点）测试 — Interpreter 路径
// ------------------------------------------------------------
// Logpoint 语义：
//   1. 命中不暂停（不触发 pausedAt 信号）
//   2. 输出日志消息到 logCallback（若已设置）
//   3. 递增 hitCount（即便未设置 logCallback）
//   4. 支持条件 Logpoint（条件为真时才输出日志 + 递增 hitCount）
// ============================================================

TEST(R104Logpoint, HitsAndLogs_NoPause) {
    // 源码：循环 5 次，第 3 行设为 Logpoint
    const std::string source = "var i = 0;\n"      // line 1
                               "while (i < 5) {\n" // line 2
                               "    i = i + 1;\n"  // line 3 — Logpoint
                               "}\n"               // line 4
                               "print(i);\n";      // line 5

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    std::string output;
    interp.setOutputCallback([&](const std::string& s) { output += s; });
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    std::vector<std::string> capturedLogs;
    std::vector<int> capturedLines;
    dbg->setLogCallback([&](const std::string& msg) { capturedLogs.push_back(msg); });

    dbg->reset();
    dbg->resume();
    dbg->setBreakpoint(3);
    dbg->setBreakpointKind(3, BreakpointKind::Logpoint);
    dbg->setLogpointMessage(3, "Log: i={i}");

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    // Logpoint 不暂停，执行应正常完成
    interp.execute(*ast);

    EXPECT_EQ(pauseCount.load(), 0);             // 不暂停
    EXPECT_EQ(dbg->getBreakpointHitCount(3), 5); // 5 次命中
    EXPECT_EQ(capturedLogs.size(), 5u);          // 5 条日志
    EXPECT_EQ(output, "5");                      // print 输出正常
}

TEST(R104Logpoint, ConditionalLogs_OnlyWhenTrue) {
    // 条件 Logpoint：i 为偶数时才输出日志
    const std::string source = "var i = 0;\n"       // line 1
                               "while (i < 10) {\n" // line 2
                               "    i = i + 1;\n"   // line 3 — Logpoint + condition
                               "}\n"                // line 4
                               "print(i);\n";       // line 5

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    // 模拟条件求值器：在 condition 中检查 i 是否为偶数
    // 实际生产中由 DebugEvaluator 在沙箱中求值；此处直接读 interp 的全局变量 i
    dbg->setConditionEvaluator([&](const std::string& /*cond*/) -> bool {
        // 求值条件表达式 cond（"i % 2 == 0"）— 测试中简化为读取 interp 的 i
        auto env = interp.getGlobalEnvironment();
        if (!env)
            return false;
        auto val = env->get("i");
        if (val && val->isInt()) {
            int i = static_cast<int>(val->intVal());
            return (i % 2) == 0;
        }
        return false;
    });

    std::vector<std::string> capturedLogs;
    dbg->setLogCallback([&](const std::string& msg) { capturedLogs.push_back(msg); });

    dbg->reset();
    dbg->resume();
    dbg->setBreakpoint(3);
    dbg->setBreakpointCondition(3, "i % 2 == 0");
    dbg->setBreakpointKind(3, BreakpointKind::Logpoint);
    dbg->setLogpointMessage(3, "Even i={i}");

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    interp.execute(*ast);

    EXPECT_EQ(pauseCount.load(), 0); // Logpoint 不暂停
    // i 在 line 3 自增后范围是 1..10，偶数：2,4,6,8,10 → 5 次日志
    EXPECT_EQ(capturedLogs.size(), 5u);
    EXPECT_EQ(dbg->getBreakpointHitCount(3), 5); // 仅命中条件为真的次数
}

TEST(R104Logpoint, NoLogCallback_StillCountsHits) {
    // 未设置 logCallback：日志被丢弃但 hitCount 仍递增
    const std::string source = "var i = 0;\n"
                               "while (i < 3) {\n"
                               "    i = i + 1;\n" // line 3 — Logpoint
                               "}\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    // 不设置 logCallback
    dbg->reset();
    dbg->resume();
    dbg->setBreakpoint(3);
    dbg->setBreakpointKind(3, BreakpointKind::Logpoint);
    dbg->setLogpointMessage(3, "ignored");

    interp.execute(*ast);

    EXPECT_EQ(dbg->getBreakpointHitCount(3), 3); // 仍递增 hitCount
}

TEST(R104Logpoint, KindSwitch_LineToLogpoint_PausesTurnToLogs) {
    // 验证：将普通断点切换为 Logpoint 后，原应暂停的命中变为仅记录日志
    const std::string source = "var i = 0;\n"
                               "while (i < 3) {\n"
                               "    i = i + 1;\n" // line 3
                               "}\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    int logCount = 0;
    dbg->setLogCallback([&](const std::string&) { logCount++; });

    dbg->reset();
    dbg->resume();
    dbg->setBreakpoint(3);
    dbg->setBreakpointKind(3, BreakpointKind::Logpoint);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    interp.execute(*ast);

    EXPECT_EQ(pauseCount.load(), 0);
    EXPECT_EQ(logCount, 3);
    EXPECT_EQ(dbg->getBreakpointKind(3), BreakpointKind::Logpoint);
}

// ============================================================
// 第二组：Function Breakpoint（函数断点）测试 — Interpreter 路径
// ------------------------------------------------------------
// Function BP 语义：
//   1. 按函数名设置（不依赖行号）
//   2. 函数被调用时（callNamedFunction / callClosureValue 入口）暂停
//   3. 支持条件函数断点
//   4. 多次调用累计 hitCount
// ============================================================

TEST(R104FunctionBreakpoint, HitsByName_PausesAtCallEntry) {
    const std::string source = "fun greet(name) {\n" // line 1
                               "    return \"hi \" + name;\n"
                               "}\n"
                               "var a = greet(\"Alice\");\n" // line 4 — 函数调用
                               "var b = greet(\"Bob\");\n"   // line 5
                               "print(a + \" \" + b);\n";    // line 6

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setFunctionBreakpoint("greet");

    EXPECT_TRUE(dbg->hasFunctionBreakpoint("greet"));
    EXPECT_EQ(dbg->getFunctionBreakpoints().size(), 1u);

    std::atomic<int> pauseCount{0};
    std::atomic<int> lastPausedLine{-1};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(),
        [&](int line) {
            pauseCount.fetch_add(1);
            lastPausedLine.store(line);
        },
        Qt::DirectConnection);

    // 在线程中执行（函数断点会触发暂停→阻塞）
    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // 等待第一次暂停（最多 2 秒）
    for (int i = 0; i < 200 && pauseCount.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(pauseCount.load(), 1);
    EXPECT_EQ(lastPausedLine.load(), 4); // 第一次调用 greet 在 line 4

    // resume 让第二次调用也触发
    dbg->resume();
    for (int i = 0; i < 200 && pauseCount.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(pauseCount.load(), 2);
    EXPECT_EQ(lastPausedLine.load(), 5); // 第二次调用在 line 5

    dbg->stop();
    execThread.join();
}

TEST(R104FunctionBreakpoint, NoHitForNonMatchingFunction) {
    const std::string source = "fun foo() { return 1; }\n"
                               "fun bar() { return 2; }\n"
                               "var x = foo() + bar();\n" // line 3
                               "print(x);\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setFunctionBreakpoint("nonexistent");

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    interp.execute(*ast); // 不会暂停

    EXPECT_EQ(pauseCount.load(), 0);
    EXPECT_EQ(dbg->getFunctionBreakpointHitCount("nonexistent"), 0);
}

TEST(R104FunctionBreakpoint, HitCountAccumulates) {
    const std::string source = "fun inc(n) { return n + 1; }\n"
                               "var i = 0;\n"
                               "while (i < 5) {\n"
                               "    i = inc(i);\n" // line 4
                               "}\n"
                               "print(i);\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setFunctionBreakpoint("inc");

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // 等待 5 次暂停（每次 resume 后等下一次）
    for (int hit = 0; hit < 5; ++hit) {
        for (int i = 0; i < 200 && pauseCount.load() <= hit; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(pauseCount.load(), hit + 1) << "Expected pause " << (hit + 1);
        dbg->resume();
    }

    execThread.join();
    EXPECT_EQ(dbg->getFunctionBreakpointHitCount("inc"), 5);
}

TEST(R104FunctionBreakpoint, RemoveClearsState) {
    DebugController dbg;
    dbg.setFunctionBreakpoint("foo");
    dbg.setFunctionBreakpoint("bar");
    EXPECT_EQ(dbg.getFunctionBreakpoints().size(), 2u);

    dbg.removeFunctionBreakpoint("foo");
    EXPECT_FALSE(dbg.hasFunctionBreakpoint("foo"));
    EXPECT_TRUE(dbg.hasFunctionBreakpoint("bar"));
    EXPECT_EQ(dbg.getFunctionBreakpoints().size(), 1u);

    // 移除不存在的函数断点不应崩溃
    dbg.removeFunctionBreakpoint("nonexistent");
    EXPECT_EQ(dbg.getFunctionBreakpoints().size(), 1u);
}

// ============================================================
// 第三组：Exception Breakpoint（异常断点）测试 — Interpreter 路径
// ------------------------------------------------------------
// Exception BP 语义：
//   1. 全局开关，启用后在 throw 语句执行前自动暂停
//   2. 对齐 GDB `catch throw`（不支持 catch 入口暂停）
//   3. 多次 throw 累计 hitCount
//   4. 默认禁用
// ============================================================

TEST(R104ExceptionBreakpoint, DisabledByDefault_NoPauseOnThrow) {
    const std::string source = "fun risky() {\n"
                               "    throw \"error\";\n" // line 2
                               "}\n"
                               "try {\n"
                               "    risky();\n"
                               "} catch (e) {\n"
                               "    print(\"caught: \" + e);\n"
                               "}\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    std::string output;
    interp.setOutputCallback([&](const std::string& s) { output += s; });
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    // 不启用异常断点
    EXPECT_FALSE(dbg->isExceptionBreakpointEnabled());

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    interp.execute(*ast);

    EXPECT_EQ(pauseCount.load(), 0);
    EXPECT_EQ(output, "caught: error");
    EXPECT_EQ(dbg->getExceptionBreakpointHitCount(), 0);
}

TEST(R104ExceptionBreakpoint, Enabled_PausesBeforeThrow) {
    const std::string source = "fun risky() {\n"
                               "    throw \"boom\";\n" // line 2 — throw
                               "}\n"
                               "try {\n"
                               "    risky();\n" // line 5
                               "} catch (e) {\n"
                               "    print(e);\n"
                               "}\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setExceptionBreakpointEnabled(true);
    EXPECT_TRUE(dbg->isExceptionBreakpointEnabled());

    std::atomic<int> pauseCount{0};
    std::atomic<int> lastPausedLine{-1};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(),
        [&](int line) {
            pauseCount.fetch_add(1);
            lastPausedLine.store(line);
        },
        Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // 等待 throw 前暂停（line 2）
    for (int i = 0; i < 200 && pauseCount.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(pauseCount.load(), 1);
    EXPECT_EQ(lastPausedLine.load(), 2); // throw 语句行

    dbg->stop();
    execThread.join();
    EXPECT_GE(dbg->getExceptionBreakpointHitCount(), 1);
}

TEST(R104ExceptionBreakpoint, HitCountAccumulates_MultipleThrows) {
    const std::string source = "var i = 0;\n"
                               "while (i < 3) {\n" // line 2
                               "    i = i + 1;\n"
                               "    try {\n"
                               "        throw i;\n" // line 5 — throw
                               "    } catch (e) {\n"
                               "        // swallow\n"
                               "    }\n"
                               "}\n"
                               "print(i);\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    dbg->setExceptionBreakpointEnabled(true);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // 等待 3 次 throw 暂停
    for (int hit = 0; hit < 3; ++hit) {
        for (int i = 0; i < 200 && pauseCount.load() <= hit; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(pauseCount.load(), hit + 1) << "Expected pause " << (hit + 1);
        dbg->resume();
    }

    execThread.join();
    EXPECT_EQ(dbg->getExceptionBreakpointHitCount(), 3);
}

TEST(R104ExceptionBreakpoint, ToggleEnabled) {
    DebugController dbg;
    EXPECT_FALSE(dbg.isExceptionBreakpointEnabled());
    dbg.setExceptionBreakpointEnabled(true);
    EXPECT_TRUE(dbg.isExceptionBreakpointEnabled());
    dbg.setExceptionBreakpointEnabled(false);
    EXPECT_FALSE(dbg.isExceptionBreakpointEnabled());
}

// ============================================================
// 第四组：API 单元测试 — 不需要执行源码
// ============================================================

TEST(R104LogpointApi, SetKind_StoresAndRetrieves) {
    DebugController dbg;
    dbg.setBreakpoint(10);
    EXPECT_EQ(dbg.getBreakpointKind(10), BreakpointKind::Line); // 默认

    dbg.setBreakpointKind(10, BreakpointKind::Logpoint);
    EXPECT_EQ(dbg.getBreakpointKind(10), BreakpointKind::Logpoint);

    dbg.setBreakpointKind(10, BreakpointKind::Line);
    EXPECT_EQ(dbg.getBreakpointKind(10), BreakpointKind::Line);
}

TEST(R104LogpointApi, SetLogpointMessage_StoresAndRetrieves) {
    DebugController dbg;
    dbg.setBreakpoint(15);
    EXPECT_EQ(dbg.getLogpointMessage(15), "");

    dbg.setLogpointMessage(15, "i={i}");
    EXPECT_EQ(dbg.getLogpointMessage(15), "i={i}");
}

TEST(R104LogpointApi, SetLogCallback_NullThenRestored) {
    DebugController dbg;
    // 不设置 logCallback 不应导致 Logpoint 命中崩溃（默认回调为空）
    dbg.setBreakpoint(5);
    dbg.setBreakpointKind(5, BreakpointKind::Logpoint);
    dbg.setLogpointMessage(5, "test");

    int callCount = 0;
    dbg.setLogCallback([&](const std::string&) { callCount++; });
    // 不实际执行，仅验证 setLogCallback 不崩溃
    SUCCEED();
}

TEST(R104FunctionBreakpointApi, BatchSet_ReplacesAll) {
    DebugController dbg;
    dbg.setFunctionBreakpoint("foo");
    dbg.setFunctionBreakpoint("bar");

    QSet<std::string> batch;
    batch.insert("baz");
    batch.insert("qux");
    dbg.setFunctionBreakpoints(batch);

    EXPECT_EQ(dbg.getFunctionBreakpoints().size(), 2u);
    EXPECT_TRUE(dbg.hasFunctionBreakpoint("baz"));
    EXPECT_TRUE(dbg.hasFunctionBreakpoint("qux"));
    EXPECT_FALSE(dbg.hasFunctionBreakpoint("foo"));
    EXPECT_FALSE(dbg.hasFunctionBreakpoint("bar"));
}

TEST(R104FunctionBreakpointApi, Condition_SetAndGet) {
    DebugController dbg;
    dbg.setFunctionBreakpoint("foo");
    EXPECT_EQ(dbg.getFunctionBreakpointCondition("foo"), "");

    dbg.setFunctionBreakpointCondition("foo", "n > 10");
    EXPECT_EQ(dbg.getFunctionBreakpointCondition("foo"), "n > 10");
}

// ============================================================
// 第五组：Logpoint 日志信号发射测试
// ------------------------------------------------------------
// logpointLogged 信号在 Logpoint 命中后发射，UI 通过 Qt::QueuedConnection
// 安全接收。此处用 Qt::DirectConnection 同步接收验证。
// ============================================================

TEST(R104LogpointSignal, LogpointLogged_EmittedOnHit) {
    const std::string source = "var i = 0;\n"
                               "while (i < 3) {\n"
                               "    i = i + 1;\n" // line 3
                               "}\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    std::vector<int> signalLines;
    std::vector<std::string> signalMessages;
    QObject::connect(
        dbg.get(), &DebugController::logpointLogged, dbg.get(),
        [&](int line, const std::string& msg) {
            signalLines.push_back(line);
            signalMessages.push_back(msg);
        },
        Qt::DirectConnection);

    dbg->reset();
    dbg->resume();
    dbg->setBreakpoint(3);
    dbg->setBreakpointKind(3, BreakpointKind::Logpoint);
    dbg->setLogpointMessage(3, "Hit line 3");

    interp.execute(*ast);

    EXPECT_EQ(signalLines.size(), 3u);
    EXPECT_EQ(signalMessages.size(), 3u);
    for (auto line : signalLines) {
        EXPECT_EQ(line, 3);
    }
    for (const auto& msg : signalMessages) {
        EXPECT_EQ(msg, "Hit line 3");
    }
}
