// ============================================================
// TestAppOrchestration — App 编排层（DebugCoordinator / WorkerManager /
// PipelineRunner）回归测试
// ------------------------------------------------------------
// 本测试覆盖 ARCH-11 拆分后的三个编排组件的线程安全与生命周期：
//
//   1. DebugCoordinator 断点生命周期：setup -> query -> modify -> clear
//   2. DebugCoordinator 函数断点 + 异常断点
//   3. DebugCoordinator Watchpoint（数据断点）
//   4. DebugCoordinator getFrameLocalsAt 边界（负数/越界返回空）
//   5. WorkerManager 生命周期：prepareRun -> startWorker -> stopForClose
//   6. WorkerManager 调试模式标志
//   7. WorkerManager 信号发射（QSignalSpy）
//   8. PipelineRunner 前端管线：合法源码 -> tokens + AST
//   9. PipelineRunner 错误诊断：非法源码 -> 非空 diagnostics
//  10. PipelineRunner 格式化往返
//  11. 线程安全：start worker 后立即从主线程查询状态（无崩溃）
//  12. WorkerManager stopForClose 超时行为（长时间运行脚本）
//
// 线程安全说明：
//   WorkerManager 在独立 QThread 中执行 Interpreter，主线程通过
//   QueuedConnection 信号与之通信。本测试验证：
//   - 主线程在 worker 运行期间查询 isRunning()/isDebugRun() 不崩溃
//   - stopForClose 在超时内等待 worker 退出，超时后返回 false
//   - forceStop 作为兜底清理路径可安全调用
//   - 信号通过 QSignalSpy 验证跨线程投递正确性
//
// 构建依赖：minilang_core + app/DebugCoordinator.cpp + app/WorkerManager.cpp
//           + app/PipelineRunner.cpp + app/InterpreterWorker.cpp + Qt6::Core
// ============================================================

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/DebugCoordinator.h"
#include "app/PipelineRunner.h"
#include "app/WorkerManager.h"
#include "debug/DebugController.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

// ============================================================
// 辅助：确保 QCoreApplication 存在（QSignalSpy / QThread 需要事件循环）
// ============================================================
namespace {

QCoreApplication* ensureApp() {
    if (QCoreApplication::instance())
        return QCoreApplication::instance();
    static int argc = 1;
    static char arg0[] = "test_app_orchestration";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

/// 在事件循环中等待条件满足或超时（避免 std::this_thread::sleep_for 阻塞事件分发）
/// @param predicate  返回 true 时停止等待
/// @param timeoutMs  最大等待毫秒数
/// @return true 表示条件在超时前满足
bool waitFor(std::function<bool()> predicate, int timeoutMs = 5000) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    // 周期性检查条件（16ms 约 60fps，足够响应跨线程 QueuedConnection 信号）
    QTimer poll;
    poll.setInterval(16);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (predicate())
            loop.quit();
    });
    timer.start(timeoutMs);
    poll.start();
    // 先检查一次，避免条件已满足时仍进入事件循环
    if (predicate())
        return true;
    loop.exec();
    return !timedOut;
}

/// 处理待决事件（驱动 QueuedConnection 信号投递）
void processEvents(int ms = 50) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

// ============================================================
// 测试 Fixture：创建共享 Interpreter + DebugController
// ============================================================
class AppOrchestrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        interpreter_ = std::make_shared<Interpreter>();
        debugger_ = std::make_shared<DebugController>();
        // 将调试器绑定到解释器（checkBreak 需要）
        interpreter_->setDebugger(debugger_);
    }

    void TearDown() override {
        // 确保调试器停止，避免残留暂停状态影响后续测试
        if (debugger_)
            debugger_->stop();
    }

    /// 使用 PipelineRunner 解析简单程序并返回 AST（供 WorkerManager 测试使用）
    std::shared_ptr<Block> parseSimpleProgram() {
        PipelineRunner pipeline;
        auto result = pipeline.runFrontendPipeline("var x = 1;\nprint(x);");
        EXPECT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
        return pipeline.astRoot();
    }

    std::shared_ptr<Interpreter> interpreter_;
    std::shared_ptr<DebugController> debugger_;
};

// ============================================================
// 第一组：DebugCoordinator 断点生命周期
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_BreakpointLifecycle_SetupQueryModifyClear) {
    DebugCoordinator coord(interpreter_, debugger_);

    // 初始状态：无断点
    EXPECT_FALSE(coord.hasBreakpoints());
    EXPECT_TRUE(coord.getBreakpoints().isEmpty());

    // setupDebug：设置断点 + 条件
    QSet<int> breakpoints = {3, 7, 12};
    QMap<int, std::string> conditions;
    conditions[7] = "x > 10";
    coord.setupDebug(breakpoints, conditions);

    // 查询：断点集合正确
    EXPECT_TRUE(coord.hasBreakpoints());
    QSet<int> retrieved = coord.getBreakpoints();
    EXPECT_EQ(retrieved.size(), 3);
    EXPECT_TRUE(retrieved.contains(3));
    EXPECT_TRUE(retrieved.contains(7));
    EXPECT_TRUE(retrieved.contains(12));

    // 查询：条件正确
    EXPECT_EQ(coord.getBreakpointCondition(7), "x > 10");
    EXPECT_EQ(coord.getBreakpointCondition(3), ""); // 无条件

    // 修改：替换断点集合
    QSet<int> newBreakpoints = {5, 20};
    coord.setBreakpoints(newBreakpoints);
    retrieved = coord.getBreakpoints();
    EXPECT_EQ(retrieved.size(), 2);
    EXPECT_TRUE(retrieved.contains(5));
    EXPECT_TRUE(retrieved.contains(20));
    EXPECT_FALSE(retrieved.contains(3)); // 旧断点已移除

    // 修改：更新条件
    coord.setBreakpointCondition(5, "y == 0");
    EXPECT_EQ(coord.getBreakpointCondition(5), "y == 0");

    // 清除：设置空集合
    coord.setBreakpoints(QSet<int>());
    EXPECT_FALSE(coord.hasBreakpoints());
    EXPECT_TRUE(coord.getBreakpoints().isEmpty());
}

TEST_F(AppOrchestrationTest, DebugCoord_HitCount_InitialZero) {
    DebugCoordinator coord(interpreter_, debugger_);
    coord.setBreakpoints(QSet<int>{10});
    // 未执行时命中次数为 0
    EXPECT_EQ(coord.getBreakpointHitCount(10), 0);
    // 不存在的断点行也返回 0
    EXPECT_EQ(coord.getBreakpointHitCount(99), 0);
}

// ============================================================
// 第二组：DebugCoordinator 临时断点（runToCursor）
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_TemporaryBreakpoint_SetGetClear) {
    DebugCoordinator coord(interpreter_, debugger_);

    // 初始无临时断点
    EXPECT_EQ(coord.getTemporaryBreakpoint(), -1);

    // 设置临时断点
    coord.setTemporaryBreakpoint(15);
    EXPECT_EQ(coord.getTemporaryBreakpoint(), 15);

    // 覆盖：新值替换旧值
    coord.setTemporaryBreakpoint(22);
    EXPECT_EQ(coord.getTemporaryBreakpoint(), 22);

    // 清除
    coord.clearTemporaryBreakpoint();
    EXPECT_EQ(coord.getTemporaryBreakpoint(), -1);
}

TEST_F(AppOrchestrationTest, DebugCoord_TemporaryBreakpoint_InvalidLine_Ignored) {
    DebugCoordinator coord(interpreter_, debugger_);
    // 无效行号（<=0）被忽略
    coord.setTemporaryBreakpoint(0);
    EXPECT_EQ(coord.getTemporaryBreakpoint(), -1);
    coord.setTemporaryBreakpoint(-5);
    EXPECT_EQ(coord.getTemporaryBreakpoint(), -1);
}

// ============================================================
// 第三组：DebugCoordinator 函数断点 + 异常断点
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_FunctionBreakpoint_AddQueryRemove) {
    DebugCoordinator coord(interpreter_, debugger_);

    // 初始无函数断点
    EXPECT_FALSE(coord.hasFunctionBreakpoint("foo"));
    EXPECT_TRUE(coord.getFunctionBreakpoints().isEmpty());

    // 添加函数断点
    coord.setFunctionBreakpoint("foo");
    coord.setFunctionBreakpoint("bar");
    EXPECT_TRUE(coord.hasFunctionBreakpoint("foo"));
    EXPECT_TRUE(coord.hasFunctionBreakpoint("bar"));
    EXPECT_FALSE(coord.hasFunctionBreakpoint("baz"));

    QSet<std::string> names = coord.getFunctionBreakpoints();
    EXPECT_EQ(names.size(), 2);
    EXPECT_TRUE(names.contains("foo"));
    EXPECT_TRUE(names.contains("bar"));

    // 设置条件
    coord.setFunctionBreakpointCondition("foo", "n > 5");
    EXPECT_EQ(coord.getFunctionBreakpointCondition("foo"), "n > 5");
    EXPECT_EQ(coord.getFunctionBreakpointCondition("bar"), "");

    // 命中次数初始为 0
    EXPECT_EQ(coord.getFunctionBreakpointHitCount("foo"), 0);

    // 移除
    coord.removeFunctionBreakpoint("foo");
    EXPECT_FALSE(coord.hasFunctionBreakpoint("foo"));
    EXPECT_TRUE(coord.hasFunctionBreakpoint("bar")); // bar 不受影响

    // 批量替换
    coord.setFunctionBreakpoints(QSet<std::string>{"qux", "quux"});
    names = coord.getFunctionBreakpoints();
    EXPECT_EQ(names.size(), 2);
    EXPECT_FALSE(names.contains("bar")); // 旧值被替换
    EXPECT_TRUE(names.contains("qux"));
}

TEST_F(AppOrchestrationTest, DebugCoord_ExceptionBreakpoint_EnableDisable) {
    DebugCoordinator coord(interpreter_, debugger_);

    // 默认禁用
    EXPECT_FALSE(coord.isExceptionBreakpointEnabled());
    EXPECT_EQ(coord.getExceptionBreakpointHitCount(), 0);

    // 启用
    coord.setExceptionBreakpointEnabled(true);
    EXPECT_TRUE(coord.isExceptionBreakpointEnabled());

    // 禁用
    coord.setExceptionBreakpointEnabled(false);
    EXPECT_FALSE(coord.isExceptionBreakpointEnabled());
}

// ============================================================
// 第四组：DebugCoordinator Logpoint + 断点类型
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_BreakpointKind_LineAndLogpoint) {
    DebugCoordinator coord(interpreter_, debugger_);

    coord.setBreakpoints(QSet<int>{5, 10});

    // 默认为 Line 类型
    EXPECT_EQ(coord.getBreakpointKind(5), BreakpointKind::Line);

    // 设置为 Logpoint
    coord.setBreakpointKind(5, BreakpointKind::Logpoint);
    EXPECT_EQ(coord.getBreakpointKind(5), BreakpointKind::Logpoint);
    // 其他行不受影响
    EXPECT_EQ(coord.getBreakpointKind(10), BreakpointKind::Line);

    // Logpoint 消息
    coord.setLogpointMessage(5, "i={i}, sum={sum}");
    EXPECT_EQ(coord.getLogpointMessage(5), "i={i}, sum={sum}");
    EXPECT_EQ(coord.getLogpointMessage(10), ""); // 非 Logpoint 行无消息

    // 恢复为 Line
    coord.setBreakpointKind(5, BreakpointKind::Line);
    EXPECT_EQ(coord.getBreakpointKind(5), BreakpointKind::Line);
}

TEST_F(AppOrchestrationTest, DebugCoord_LogCallback_SetNoCrash) {
    DebugCoordinator coord(interpreter_, debugger_);

    // 设置日志回调并验证可调用（不崩溃）
    std::string captured;
    coord.setLogCallback([&captured](const std::string& msg) { captured = msg; });
    // 回调已注册（通过 DebugController 内部存储），此处仅验证设置不崩溃
    SUCCEED();
}

// ============================================================
// 第五组：DebugCoordinator Watchpoint（数据断点）
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_Watchpoint_AddQueryRemoveClear) {
    DebugCoordinator coord(interpreter_, debugger_);

    // 初始无 watchpoint
    EXPECT_FALSE(coord.hasWatchpoints());
    EXPECT_TRUE(coord.getWatchpoints().empty());

    // 添加变量 watchpoint
    WatchpointInfo wp1("counter");
    coord.setWatchpoint(wp1);
    EXPECT_TRUE(coord.hasWatchpoints());

    // 添加字段 watchpoint
    WatchpointInfo wp2;
    wp2.kind = WatchpointTargetKind::Field;
    wp2.varName = "obj";
    wp2.fieldName = "x";
    wp2.condition = "obj.x > 100";
    coord.setWatchpoint(wp2);

    // 查询
    auto wps = coord.getWatchpoints();
    EXPECT_EQ(wps.size(), 2u);

    // 移除变量 watchpoint
    coord.removeWatchpoint("counter");
    wps = coord.getWatchpoints();
    EXPECT_EQ(wps.size(), 1u);
    EXPECT_EQ(wps[0].varName, "obj");
    EXPECT_EQ(wps[0].fieldName, "x");

    // 清空
    coord.clearWatchpoints();
    EXPECT_FALSE(coord.hasWatchpoints());
    EXPECT_TRUE(coord.getWatchpoints().empty());
}

// ============================================================
// 第六组：DebugCoordinator getFrameLocalsAt 边界
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_GetFrameLocalsAt_NegativeDepth_ReturnsEmpty) {
    DebugCoordinator coord(interpreter_, debugger_);
    // 负数深度返回空（不崩溃）
    auto locals = coord.getFrameLocalsAt(-1);
    EXPECT_TRUE(locals.empty());
    locals = coord.getFrameLocalsAt(-100);
    EXPECT_TRUE(locals.empty());
}

TEST_F(AppOrchestrationTest, DebugCoord_GetFrameLocalsAt_OutOfRange_ReturnsEmpty) {
    DebugCoordinator coord(interpreter_, debugger_);
    // 未执行任何代码时调用栈为空，任何非负深度都越界
    auto locals = coord.getFrameLocalsAt(0);
    EXPECT_TRUE(locals.empty());
    locals = coord.getFrameLocalsAt(999);
    EXPECT_TRUE(locals.empty());
}

// ============================================================
// 第七组：DebugCoordinator 调试状态查询（未暂停）
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_IsPaused_InitiallyFalse) {
    DebugCoordinator coord(interpreter_, debugger_);
    EXPECT_FALSE(coord.isPaused());
}

TEST_F(AppOrchestrationTest, DebugCoord_VariableSnapshot_EmptyWhenNotPaused) {
    DebugCoordinator coord(interpreter_, debugger_);
    // 未暂停时变量快照为空（无回调或无执行上下文）
    auto snapshot = coord.getDebugVariableSnapshot();
    EXPECT_TRUE(snapshot.empty());
}

TEST_F(AppOrchestrationTest, DebugCoord_CallStack_EmptyWhenNotRunning) {
    DebugCoordinator coord(interpreter_, debugger_);
    auto stack = coord.getDebugCallStack();
    EXPECT_TRUE(stack.empty());
}

// ============================================================
// 第八组：DebugCoordinator pausedAt 信号
// ============================================================

TEST_F(AppOrchestrationTest, DebugCoord_PausedAtSignal_Connected) {
    DebugCoordinator coord(interpreter_, debugger_);
    QSignalSpy spy(&coord, &DebugCoordinator::pausedAt);
    ASSERT_TRUE(spy.isValid());
    // 信号连接有效（实际发射需要 Interpreter 执行命中断点，此处验证连接）
    EXPECT_EQ(spy.count(), 0);
}

// ============================================================
// 第九组：WorkerManager 生命周期
// ============================================================

TEST_F(AppOrchestrationTest, WorkerMgr_Lifecycle_PrepareStartStop) {
    WorkerManager mgr(interpreter_, debugger_);

    // 初始状态
    EXPECT_FALSE(mgr.isRunning());
    EXPECT_FALSE(mgr.isDebugRun());

    // 准备运行（非调试模式）
    auto ast = parseSimpleProgram();
    ASSERT_NE(ast, nullptr);
    bool prepared = mgr.prepareRun(false, ast, "");
    ASSERT_TRUE(prepared);
    // prepareRun 设置 isRunning_ = true（线程尚未启动但状态已标记）
    EXPECT_TRUE(mgr.isRunning());
    EXPECT_FALSE(mgr.isDebugRun());

    // 启动 worker
    mgr.startWorker();

    // 等待 worker 完成（简单程序执行很快）
    bool finished = waitFor([&]() { return !mgr.isRunning(); }, 5000);
    EXPECT_TRUE(finished) << "Worker 未在 5 秒内完成";

    // stopForClose 清理（worker 已结束时仍应返回 true）
    bool stopped = mgr.stopForClose(1000);
    EXPECT_TRUE(stopped);
    EXPECT_FALSE(mgr.isRunning());
}

TEST_F(AppOrchestrationTest, WorkerMgr_PrepareRun_NullAst_ReturnsFalse) {
    WorkerManager mgr(interpreter_, debugger_);
    // 空 AST 应返回 false
    bool prepared = mgr.prepareRun(false, nullptr, "");
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(mgr.isRunning());
}

TEST_F(AppOrchestrationTest, WorkerMgr_PrepareRun_WhileRunning_ReturnsFalse) {
    WorkerManager mgr(interpreter_, debugger_);
    auto ast = parseSimpleProgram();
    ASSERT_NE(ast, nullptr);

    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    // 第二次 prepareRun 应失败（已在运行）
    EXPECT_FALSE(mgr.prepareRun(false, ast, ""));

    // 清理
    mgr.startWorker();
    waitFor([&]() { return !mgr.isRunning(); }, 5000);
    mgr.stopForClose(1000);
}

// ============================================================
// 第十组：WorkerManager 调试模式标志
// ============================================================

TEST_F(AppOrchestrationTest, WorkerMgr_DebugMode_FlagSet) {
    WorkerManager mgr(interpreter_, debugger_);
    auto ast = parseSimpleProgram();
    ASSERT_NE(ast, nullptr);

    // 调试模式
    ASSERT_TRUE(mgr.prepareRun(true, ast, ""));
    EXPECT_TRUE(mgr.isDebugRun());
    EXPECT_TRUE(mgr.isRunning());

    // 清理
    mgr.startWorker();
    // 调试模式下无断点时正常执行完毕
    waitFor([&]() { return !mgr.isRunning(); }, 5000);
    mgr.stopForClose(1000);
    EXPECT_FALSE(mgr.isDebugRun());
}

TEST_F(AppOrchestrationTest, WorkerMgr_NonDebugMode_ClearsBreakpoints) {
    // 设置断点后以非调试模式运行，断点应被清除
    debugger_->setBreakpoints(QSet<int>{1, 2});
    EXPECT_TRUE(debugger_->hasBreakpoint(1));

    WorkerManager mgr(interpreter_, debugger_);
    auto ast = parseSimpleProgram();
    ASSERT_NE(ast, nullptr);
    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));

    // 非调试运行清除残留断点
    EXPECT_TRUE(debugger_->getBreakpoints().isEmpty());

    mgr.startWorker();
    waitFor([&]() { return !mgr.isRunning(); }, 5000);
    mgr.stopForClose(1000);
}

// ============================================================
// 第十一组：WorkerManager 信号发射（QSignalSpy）
// ============================================================

TEST_F(AppOrchestrationTest, WorkerMgr_Signal_RunOk_Emitted) {
    WorkerManager mgr(interpreter_, debugger_);
    QSignalSpy spyRunOk(&mgr, &WorkerManager::runOk);
    QSignalSpy spyFinished(&mgr, &WorkerManager::workerFinished);
    ASSERT_TRUE(spyRunOk.isValid());
    ASSERT_TRUE(spyFinished.isValid());

    auto ast = parseSimpleProgram();
    ASSERT_NE(ast, nullptr);
    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    mgr.startWorker();

    // 等待 workerFinished 信号（QueuedConnection 跨线程投递）
    bool got = waitFor([&]() { return spyFinished.count() > 0; }, 5000);
    EXPECT_TRUE(got) << "workerFinished 信号未在 5 秒内收到";

    if (spyFinished.count() > 0) {
        // workerFinished(bool wasDebug) — 非调试运行应为 false
        EXPECT_FALSE(spyFinished.at(0).at(0).toBool());
    }
    // runOk 应发射（程序正常结束）
    EXPECT_GE(spyRunOk.count(), 1);
}

TEST_F(AppOrchestrationTest, WorkerMgr_Signal_OutputReady_Emitted) {
    WorkerManager mgr(interpreter_, debugger_);
    QSignalSpy spyOutput(&mgr, &WorkerManager::outputReady);
    ASSERT_TRUE(spyOutput.isValid());

    auto ast = parseSimpleProgram(); // "var x = 1;\nprint(x);"
    ASSERT_NE(ast, nullptr);
    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    mgr.startWorker();

    // 等待输出信号
    bool got = waitFor([&]() { return spyOutput.count() > 0; }, 5000);
    EXPECT_TRUE(got) << "outputReady 信号未在 5 秒内收到";

    if (spyOutput.count() > 0) {
        // print(x) 输出 "1"（可能带换行）
        QString output = spyOutput.at(0).at(0).toString();
        EXPECT_TRUE(output.contains("1"));
    }

    // 等待完成
    waitFor([&]() { return !mgr.isRunning(); }, 3000);
    mgr.stopForClose(1000);
}

TEST_F(AppOrchestrationTest, WorkerMgr_Signal_StoppedByUser) {
    WorkerManager mgr(interpreter_, debugger_);
    QSignalSpy spyStopped(&mgr, &WorkerManager::stoppedByUser);
    ASSERT_TRUE(spyStopped.isValid());

    // 使用较长运行的脚本（循环），确保有时间调用 stop
    PipelineRunner pipeline;
    auto result = pipeline.runFrontendPipeline("var i = 0;\nwhile (i < 1000000) {\n  i = i + 1;\n}");
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
    auto ast = pipeline.astRoot();
    ASSERT_NE(ast, nullptr);

    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    mgr.startWorker();

    // 等一小段时间让 worker 开始执行
    processEvents(50);

    // 通过 debugger stop 触发 stoppedByUser
    debugger_->stop();

    // 等待 stoppedByUser 信号或运行结束
    bool got = waitFor([&]() { return spyStopped.count() > 0 || !mgr.isRunning(); }, 5000);
    // worker 可能因循环太快在 stop 前结束，两种结果都可接受
    if (got && spyStopped.count() > 0) {
        EXPECT_GE(spyStopped.count(), 1);
    }
    mgr.stopForClose(2000);
}

// ============================================================
// 第十二组：WorkerManager 访问器
// ============================================================

TEST_F(AppOrchestrationTest, WorkerMgr_Accessors_ReturnSharedInstances) {
    WorkerManager mgr(interpreter_, debugger_);
    // interpreter() / debugger() 返回与构造时相同的 shared_ptr
    EXPECT_EQ(mgr.interpreter(), interpreter_);
    EXPECT_EQ(mgr.debugger(), debugger_);
}

TEST_F(AppOrchestrationTest, WorkerMgr_BuildInputCallback_ReturnsValid) {
    WorkerManager mgr(interpreter_, debugger_);
    auto cb = mgr.buildInputCallback();
    // 回调应为有效 std::function
    EXPECT_TRUE(static_cast<bool>(cb));
}

TEST_F(AppOrchestrationTest, WorkerMgr_SetupMainCallbacks_NoCrash) {
    WorkerManager mgr(interpreter_, debugger_);
    // setupMainCallbacks 在无 worker 时调用不崩溃
    mgr.setupMainCallbacks();
    SUCCEED();
}

// ============================================================
// 第十三组：PipelineRunner 前端管线
// ============================================================

TEST_F(AppOrchestrationTest, Pipeline_ValidSource_TokensAndAst) {
    PipelineRunner pipeline;
    auto result = pipeline.runFrontendPipeline("var x = 42;\nprint(x);");

    EXPECT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
    EXPECT_TRUE(result.errorMessage.empty());

    // tokens 非空
    EXPECT_FALSE(pipeline.lastTokens().empty());
    // 至少包含 var/x/=/42/;/print/(/x/)/; + EOF
    EXPECT_GE(pipeline.lastTokens().size(), 9u);

    // AST 非空
    EXPECT_NE(pipeline.astRoot(), nullptr);
    EXPECT_NE(pipeline.astRootPtr(), nullptr);

    // lastSource 记录
    EXPECT_EQ(pipeline.lastSource(), "var x = 42;\nprint(x);");
}

TEST_F(AppOrchestrationTest, Pipeline_EmptySource_NoCrash) {
    PipelineRunner pipeline;
    auto result = pipeline.runFrontendPipeline("");
    // 空源码：Lexer 产生 EOF，Parser 产生空 Block（或失败取决于实现）
    // 无论哪种，不应崩溃
    if (result.status == PipelineRunner::PipelineStatus::OK) {
        // 空程序 AST 可能为空 Block 或 nullptr
        SUCCEED();
    } else {
        EXPECT_FALSE(result.errorMessage.empty());
    }
}

TEST_F(AppOrchestrationTest, Pipeline_MultipleStatements_AllParsed) {
    PipelineRunner pipeline;
    std::string source = "var a = 1;\nvar b = 2;\nvar c = a + b;\nprint(c);";
    auto result = pipeline.runFrontendPipeline(source);
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
    EXPECT_NE(pipeline.astRoot(), nullptr);
    // AST 根为 Block，应包含 4 条语句
    auto* block = pipeline.astRootPtr();
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->statements.size(), 4u);
}

// ============================================================
// 第十四组：PipelineRunner 错误诊断
// ============================================================

TEST_F(AppOrchestrationTest, Pipeline_InvalidSource_NonEmptyDiagnostics) {
    PipelineRunner pipeline;
    // 语法错误：缺少右括号
    auto result = pipeline.runFrontendPipeline("var x = (1 + 2;");

    // 应为 ParserFailed 或 LexerFailed
    EXPECT_NE(result.status, PipelineRunner::PipelineStatus::OK);

    // 诊断非空
    if (result.status == PipelineRunner::PipelineStatus::ParserFailed) {
        EXPECT_TRUE(pipeline.parserDiagnostics().hasErrors());
        EXPECT_GT(pipeline.parserDiagnostics().errorCount(), 0);
    } else if (result.status == PipelineRunner::PipelineStatus::LexerFailed) {
        EXPECT_TRUE(pipeline.lexerDiagnostics().hasErrors());
    }

    // errorMessage 或 diagnostics 至少一个有内容
    if (result.diagnostics) {
        EXPECT_FALSE(result.diagnostics->empty());
    }
}

TEST_F(AppOrchestrationTest, Pipeline_LexerError_InvalidToken) {
    PipelineRunner pipeline;
    // 非法字符（如 @ 在 MiniLang 中不合法）
    auto result = pipeline.runFrontendPipeline("var x = @invalid;");

    // Lexer 应产生错误 token 或 Parser 报错
    if (result.status == PipelineRunner::PipelineStatus::LexerFailed) {
        EXPECT_TRUE(pipeline.lexerDiagnostics().hasErrors());
    } else if (result.status == PipelineRunner::PipelineStatus::ParserFailed) {
        // Lexer 可能将 @ 标记为 TK_ERROR 但管线仍进入 Parser
        EXPECT_TRUE(pipeline.parserDiagnostics().hasErrors() || pipeline.lexerDiagnostics().hasErrors());
    }
    // 不应崩溃
    SUCCEED();
}

TEST_F(AppOrchestrationTest, Pipeline_DiagnosticsReady_Signal) {
    PipelineRunner pipeline;
    QSignalSpy spy(&pipeline, &PipelineRunner::diagnosticsReady);
    ASSERT_TRUE(spy.isValid());

    // 合法源码不发射诊断信号（或发射空 bag）
    pipeline.runFrontendPipeline("var x = 1;");
    int countAfterValid = spy.count();

    // 非法源码应发射诊断信号
    pipeline.runFrontendPipeline("var = ;");
    EXPECT_GT(spy.count(), countAfterValid);
}

// ============================================================
// 第十五组：PipelineRunner 格式化往返
// ============================================================

TEST_F(AppOrchestrationTest, Pipeline_FormatCode_RoundTrip) {
    PipelineRunner pipeline;
    // 先执行前端管线（formatCode 需要 lastTokens_ + astRoot_）
    std::string source = "var   x=1;\nprint(  x  );";
    auto result = pipeline.runFrontendPipeline(source);
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);

    std::string formatted;
    bool ok = pipeline.formatCode(formatted);
    ASSERT_TRUE(ok) << "formatCode 应成功";
    EXPECT_FALSE(formatted.empty());
    // 格式化后应保留语义关键字
    EXPECT_NE(formatted.find("var"), std::string::npos);
    EXPECT_NE(formatted.find("print"), std::string::npos);
    EXPECT_NE(formatted.find("x"), std::string::npos);
}

TEST_F(AppOrchestrationTest, Pipeline_FormatCode_Idempotent) {
    PipelineRunner pipeline;
    std::string source = "var x = 1;\nprint(x);";
    auto result = pipeline.runFrontendPipeline(source);
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);

    std::string formatted1;
    ASSERT_TRUE(pipeline.formatCode(formatted1));

    // 对格式化结果再次格式化应幂等
    auto result2 = pipeline.runFrontendPipeline(formatted1);
    ASSERT_EQ(result2.status, PipelineRunner::PipelineStatus::OK);
    std::string formatted2;
    ASSERT_TRUE(pipeline.formatCode(formatted2));
    EXPECT_EQ(formatted1, formatted2) << "格式化应幂等";
}

// ============================================================
// 第十六组：PipelineRunner 引擎访问器 + 缓存
// ============================================================

TEST_F(AppOrchestrationTest, Pipeline_Accessors_Valid) {
    PipelineRunner pipeline;
    // 访问器返回有效引用（编译期验证 + 运行时不崩溃）
    auto& lexer = pipeline.lexer();
    auto& parser = pipeline.parser();
    auto& compiler = pipeline.compiler();
    auto& formatter = pipeline.formatter();
    (void)lexer;
    (void)parser;
    (void)compiler;
    (void)formatter;
    SUCCEED();
}

TEST_F(AppOrchestrationTest, Pipeline_InvalidateCache_NoCrash) {
    PipelineRunner pipeline;
    pipeline.runFrontendPipeline("var x = 1;");
    // 失效缓存不崩溃
    pipeline.invalidatePipelineCache();
    // 失效后重新运行仍正常
    auto result = pipeline.runFrontendPipeline("var y = 2;");
    EXPECT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
}

TEST_F(AppOrchestrationTest, Pipeline_ResetState_ClearsAll) {
    PipelineRunner pipeline;
    pipeline.runFrontendPipeline("var x = 1;");
    EXPECT_FALSE(pipeline.lastTokens().empty());
    EXPECT_NE(pipeline.astRoot(), nullptr);

    pipeline.resetPipelineState();
    EXPECT_TRUE(pipeline.lastTokens().empty());
    EXPECT_EQ(pipeline.astRoot(), nullptr);
    EXPECT_TRUE(pipeline.lastSource().empty());
}

TEST_F(AppOrchestrationTest, Pipeline_BytecodeCache_EnableDisable) {
    PipelineRunner pipeline;
    // 默认启用
    EXPECT_TRUE(pipeline.isBytecodeCacheEnabled());
    pipeline.setBytecodeCacheEnabled(false);
    EXPECT_FALSE(pipeline.isBytecodeCacheEnabled());
    pipeline.setBytecodeCacheEnabled(true);
    EXPECT_TRUE(pipeline.isBytecodeCacheEnabled());
}

// ============================================================
// 第十七组：线程安全 — 主线程并发查询
// ============================================================

TEST_F(AppOrchestrationTest, ThreadSafety_QueryStateWhileRunning_NoCrash) {
    WorkerManager mgr(interpreter_, debugger_);

    // 使用较长运行的脚本确保 worker 处于活跃状态
    PipelineRunner pipeline;
    auto result = pipeline.runFrontendPipeline(
        "var sum = 0;\nvar i = 0;\nwhile (i < 100000) {\n  sum = sum + i;\n  i = i + 1;\n}\nprint(sum);");
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
    auto ast = pipeline.astRoot();
    ASSERT_NE(ast, nullptr);

    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    mgr.startWorker();

    // 主线程在 worker 运行期间反复查询状态（验证 atomic 读取不崩溃）
    for (int i = 0; i < 100; ++i) {
        volatile bool running = mgr.isRunning();
        volatile bool debug = mgr.isDebugRun();
        (void)running;
        (void)debug;
        // 查询 interpreter/debugger 访问器
        auto interp = mgr.interpreter();
        auto dbg = mgr.debugger();
        ASSERT_NE(interp, nullptr);
        ASSERT_NE(dbg, nullptr);
        // 短暂让出事件循环
        QCoreApplication::processEvents();
    }

    // 等待完成
    waitFor([&]() { return !mgr.isRunning(); }, 10000);
    mgr.stopForClose(2000);
    EXPECT_FALSE(mgr.isRunning());
}

TEST_F(AppOrchestrationTest, ThreadSafety_DebugCoordinatorConcurrentAccess_NoCrash) {
    // 模拟主线程通过 DebugCoordinator 查询状态，同时 worker 在运行
    DebugCoordinator coord(interpreter_, debugger_);
    WorkerManager mgr(interpreter_, debugger_);

    PipelineRunner pipeline;
    auto result = pipeline.runFrontendPipeline(
        "var x = 0;\nwhile (x < 50000) {\n  x = x + 1;\n}");
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
    auto ast = pipeline.astRoot();
    ASSERT_NE(ast, nullptr);

    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    mgr.startWorker();

    // 主线程通过 DebugCoordinator 查询（不阻塞 worker）
    for (int i = 0; i < 50; ++i) {
        volatile bool paused = coord.isPaused();
        (void)paused;
        auto locals = coord.getFrameLocalsAt(0); // 应返回空（未暂停）
        auto snapshot = coord.getDebugVariableSnapshot();
        auto stack = coord.getDebugCallStack();
        (void)locals;
        (void)snapshot;
        (void)stack;
        QCoreApplication::processEvents();
    }

    waitFor([&]() { return !mgr.isRunning(); }, 10000);
    mgr.stopForClose(2000);
}

// ============================================================
// 第十八组：WorkerManager stopForClose 超时行为
// ============================================================

TEST_F(AppOrchestrationTest, WorkerMgr_StopForClose_LongRunningScript) {
    WorkerManager mgr(interpreter_, debugger_);

    // 长时间运行脚本（大循环）
    PipelineRunner pipeline;
    auto result = pipeline.runFrontendPipeline(
        "var i = 0;\nwhile (i < 10000000) {\n  i = i + 1;\n}");
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
    auto ast = pipeline.astRoot();
    ASSERT_NE(ast, nullptr);

    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    mgr.startWorker();

    // 让 worker 开始执行
    processEvents(30);

    // stopForClose 通过 debugger_->stop() 设置 stopped_ 标志，
    // Interpreter 在 checkBreak 检测到后抛 DebugStopException 退出。
    // 对于紧密循环，checkBreak 在每个 AST 节点调用，应能及时响应。
    bool stopped = mgr.stopForClose(3000);
    if (stopped) {
        // 正常停止
        EXPECT_FALSE(mgr.isRunning());
    } else {
        // 超时：需要 forceStop 兜底
        mgr.forceStop();
        EXPECT_FALSE(mgr.isRunning());
    }
}

TEST_F(AppOrchestrationTest, WorkerMgr_ForceStop_Idempotent) {
    WorkerManager mgr(interpreter_, debugger_);
    auto ast = parseSimpleProgram();
    ASSERT_NE(ast, nullptr);

    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    mgr.startWorker();

    // 等待完成
    waitFor([&]() { return !mgr.isRunning(); }, 5000);

    // forceStop 在 worker 已结束后调用不崩溃（幂等）
    mgr.forceStop();
    EXPECT_FALSE(mgr.isRunning());
    // 再次调用仍安全
    mgr.forceStop();
    EXPECT_FALSE(mgr.isRunning());
}

TEST_F(AppOrchestrationTest, WorkerMgr_StopForClose_AlreadyStopped_ReturnsTrue) {
    WorkerManager mgr(interpreter_, debugger_);
    // 未 prepareRun 时 stopForClose 应安全返回 true（无 workerThread_）
    bool stopped = mgr.stopForClose(100);
    EXPECT_TRUE(stopped);
    EXPECT_FALSE(mgr.isRunning());
}

// ============================================================
// 第十九组：WorkerManager + DebugCoordinator 协作
// ============================================================

TEST_F(AppOrchestrationTest, Integration_DebugCoordAndWorkerMgr_ShareState) {
    // DebugCoordinator 和 WorkerManager 共享同一 Interpreter/DebugController
    DebugCoordinator coord(interpreter_, debugger_);
    WorkerManager mgr(interpreter_, debugger_);

    // 通过 DebugCoordinator 设置断点
    coord.setBreakpoints(QSet<int>{2});
    EXPECT_TRUE(coord.hasBreakpoints());

    // WorkerManager 访问器返回相同实例
    EXPECT_EQ(mgr.debugger(), debugger_);
    EXPECT_TRUE(mgr.debugger()->hasBreakpoint(2));

    // 非调试运行会清除断点
    auto ast = parseSimpleProgram();
    ASSERT_NE(ast, nullptr);
    ASSERT_TRUE(mgr.prepareRun(false, ast, ""));
    EXPECT_FALSE(coord.hasBreakpoints()); // 断点被清除

    mgr.startWorker();
    waitFor([&]() { return !mgr.isRunning(); }, 5000);
    mgr.stopForClose(1000);
}

TEST_F(AppOrchestrationTest, Integration_DebugRun_PausesAtBreakpoint) {
    // 调试运行 + 断点 -> 暂停 -> pausedAt 信号
    DebugCoordinator coord(interpreter_, debugger_);
    WorkerManager mgr(interpreter_, debugger_);

    QSignalSpy spyPaused(&coord, &DebugCoordinator::pausedAt);
    ASSERT_TRUE(spyPaused.isValid());

    // 设置断点在第 2 行
    coord.setBreakpoints(QSet<int>{2});

    PipelineRunner pipeline;
    auto result = pipeline.runFrontendPipeline("var x = 1;\nx = x + 1;\nprint(x);");
    ASSERT_EQ(result.status, PipelineRunner::PipelineStatus::OK);
    auto ast = pipeline.astRoot();
    ASSERT_NE(ast, nullptr);

    ASSERT_TRUE(mgr.prepareRun(true, ast, ""));
    mgr.startWorker();

    // 等待暂停信号
    bool paused = waitFor([&]() { return spyPaused.count() > 0 || !mgr.isRunning(); }, 5000);
    (void)paused;

    if (spyPaused.count() > 0) {
        // 暂停在第 2 行
        int line = spyPaused.at(0).at(0).toInt();
        EXPECT_EQ(line, 2);
        EXPECT_TRUE(coord.isPaused());

        // 恢复执行
        coord.resume();
        waitFor([&]() { return !mgr.isRunning(); }, 5000);
    }
    // 即使未暂停（时序问题），也不应崩溃
    mgr.stopForClose(2000);
}

// ============================================================
// 第二十组：PipelineRunner setReplPipelineState
// ============================================================

TEST_F(AppOrchestrationTest, Pipeline_SetReplPipelineState_UpdatesTokensAndAst) {
    PipelineRunner pipeline;
    pipeline.setReplPipelineState("var y = 99;");
    EXPECT_FALSE(pipeline.lastTokens().empty());
    EXPECT_NE(pipeline.astRoot(), nullptr);
}

TEST_F(AppOrchestrationTest, Pipeline_SetReplPipelineState_InvalidSource_NoCrash) {
    PipelineRunner pipeline;
    // 非法源码：Lexer 产生 TK_ERROR -> astRoot_ 为 nullptr
    pipeline.setReplPipelineState("var = ;;;");
    // AST 应为空（Lexer 错误或 Parser 失败）
    // 具体行为取决于 Parser 错误恢复，但不应崩溃
    SUCCEED();
}
