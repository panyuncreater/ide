#pragma once

#include "debug/DebugEvaluator.h" // A5 fix: 抽取条件断点求值器
#include "debug/DebugTypes.h"     // ARCH-16 fix: 共享调试公共类型
#include "interpreter/Value.h"
#include <QMap>
#include <QObject>
#include <QSet>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ============================================================
// DebugController 调试控制器
// ============================================================
// ARCH-16 fix: StepMode / VariableSnapshot / CallStackEntry / BreakpointInfo
// 已提取到 debug/DebugTypes.h，与 test_harness/debug/DebugController.h 共享。

/// 调试控制器：管理断点、步进模式和暂停
class DebugController : public QObject {
    Q_OBJECT

public:
    explicit DebugController(QObject* parent = nullptr);
    ~DebugController();

    /// 在每个 AST 节点执行前调用
    void checkBreak(class ASTNode* node);

    /// 断点管理
    void setBreakpoint(int line);
    void setBreakpoints(const QSet<int>& lines);
    void removeBreakpoint(int line);
    void toggleBreakpoint(int line);
    bool hasBreakpoint(int line) const;
    QSet<int> getBreakpoints() const;

    /// 条件断点：设置断点条件表达式
    void setBreakpointCondition(int line, const std::string& condition);

    /// 获取断点条件
    std::string getBreakpointCondition(int line) const;

    /// 获取断点命中次数
    int getBreakpointHitCount(int line) const;

    /// 设置条件表达式求值回调（由 IDE 设置，接收条件字符串，返回 bool）
    // A5 fix: 委托给 DebugEvaluator，DebugController 不再直接持有回调
    void setConditionEvaluator(std::function<bool(const std::string&)> evaluator);

    /// 步进控制
    void stepIn();
    void stepOver();
    void stepOut();
    void resume();
    void stop();

    /// 设置当前调用深度（由 Interpreter 更新）
    void setCurrentDepth(int depth);

    /// 设置变量快照回调（由主窗口设置）
    void setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb);

    /// 设置调用栈回调
    void setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb);

    /// 获取变量快照
    std::vector<VariableSnapshot> getVariableSnapshot() const;

    /// 获取调用栈
    std::vector<CallStackEntry> getCallStack() const;

    /// 是否正在运行
    bool isRunning() const;

    /// 是否处于暂停状态
    bool isPaused() const;

    /// 重置状态
    void reset();

    /// 等待所有正在执行的 variableCallback_/callStackCallback_ 完成（用于析构前安全等待）
    // AUDIT-P1 fix: 由 private 提升为 public，DebugCoordinator 析构需调用此方法
    // 等待 RCU 优雅期结束，避免 callback 持已失效 interpreter_ shared_ptr → UAF。
    void waitCallbacksIdle() const;

signals:
    /// 调试暂停在某行
    void pausedAt(int line);

    /// 调试执行结束
    void executionFinished();

    /// 变量变更通知
    void variablesChanged();

private:
    std::atomic<int> mode_{static_cast<int>(StepMode::MODE_RUN)}; // atomic for cross-thread access
    QSet<int> breakpoints_;                                       // 断点行号集合
    QMap<int, BreakpointInfo> breakpointInfos_;                   // 条件断点详情（行号→信息）
    // A5 fix: 抽取到独立 DebugEvaluator 类，DebugController 仅持有指针
    std::unique_ptr<DebugEvaluator> evaluator_{std::make_unique<DebugEvaluator>()};
    // P0-9 fix: 跨线程读写的标量字段改为 atomic，避免数据竞争
    std::atomic<int> currentDepth_{0};        // 当前调用深度（worker 写，UI 读）
    int stepOverDepth_ = 0;                   // stepOver 时的调用深度（mutex 保护）
    int stepOutDepth_ = 0;                    // stepOut 时的调用深度（mutex 保护）
    std::atomic<int> lastPausedLine_{-1};     // 上次暂停的行号（worker 写，UI 读）
    std::atomic<int> lastPausedDepth_{-1};    // 上次暂停时的调用深度
    std::atomic<int> lastSeenLine_{-1};       // C3 fix: checkBreak 上次看到的行号
    std::atomic<bool> crossedLine_{false};    // DBG-03 fix: 是否已经跨过不同行
    std::atomic<bool> crossedDeeper_{false};  // DBG-B fix: Step Over 期间是否进入了更深的调用层
    int minBreakpointLine_ = -1;              // 最小断点行号（mutex 保护，随 breakpoints_ 一起更新）
    std::atomic<bool> hasBreakpoints_{false}; // D-P1-1 fix: 原子标志位，快速路径无锁判断
    std::atomic<bool> running_{false};        // #9 fix: atomic for cross-thread access
    std::atomic<bool> stopped_{false};        // #9 fix: atomic for cross-thread access
    std::atomic<bool> paused_{false};         // atomic for cross-thread access  // 是否处于暂停状态（等待用户操作）

    // A2: 线程安全的暂停/恢复机制（替代 QEventLoop）
    mutable std::mutex pauseMutex_; // P0-9 fix: mutable 以便 const 方法加锁
    std::condition_variable pauseCV_;

    std::function<std::vector<VariableSnapshot>()> variableCallback_;
    std::function<std::vector<CallStackEntry>()> callStackCallback_;

    // AUDIT-P1 fix: 活跃 callback 计数，用于析构时等待正在执行的 callback 完成（RCU 优雅期模式）。
    // getVariableSnapshot/getCallStack 在锁外调用 cb() 期间增减此计数，
    // DebugCoordinator 析构清空 callback 后 spin-wait 直到计数归零，避免 UAF。
    mutable std::atomic<int> activeCallbackCount_{0};

    /// 暂停当前线程，等待用户操作
    void pauseExecution();

    /// 更新最小断点行号缓存
    void updateMinBreakpointLine();

    // C11 fix: checkBreak 拆分为 4 个助手方法，降低单函数复杂度
    /// 断点命中检测（MODE_RUN 模式下检查行号是否命中断点，含条件断点求值）
    bool shouldPauseAtBreakpoint(int line, const QSet<int>& localBreakpoints,
                                 const QMap<int, BreakpointInfo>& localBreakpointInfos, int snapMinBreakpointLine);
    /// 步进模式暂停检测（STEP_IN/STEP_OVER/STEP_OUT 三种模式的状态机）
    bool shouldPauseForStepping(StepMode snapMode, int line, int snapCurrentDepth, int snapStepOverDepth,
                                int snapStepOutDepth, int snapLastPausedLine, int snapLastPausedDepth,
                                bool snapCrossedDeeper);
    /// 更新行号追踪状态（lastSeenLine_/crossedLine_/crossedDeeper_）
    void updateLineTracking(int line, int snapCurrentDepth, int snapStepOverDepth, StepMode snapMode);
    /// 执行暂停（设置 paused_、emit pausedAt、阻塞等待）
    void doPause(int line, int snapCurrentDepth);
};
