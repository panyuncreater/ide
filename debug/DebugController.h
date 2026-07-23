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

    /// R104 Logpoint：设置日志输出回调（由 IDE 设置，接收日志字符串）。
    /// Logpoint 命中时调用此回调输出格式化后的日志消息。
    /// 若未设置，Logpoint 命中后日志被丢弃（仍递增 hitCount）。
    void setLogCallback(std::function<void(const std::string&)> cb);

    /// R104 Logpoint：设置断点类型（Line / Logpoint）
    void setBreakpointKind(int line, BreakpointKind kind);

    /// R104 Logpoint：获取断点类型
    BreakpointKind getBreakpointKind(int line) const;

    /// R104 Logpoint：设置 Logpoint 日志模板（如 "i={i}, sum={sum}"）。
    /// 模板中的 {expr} 占位符在沙箱中求值后替换为对应的字符串表示。
    void setLogpointMessage(int line, const std::string& msg);

    /// R104 Logpoint：获取 Logpoint 日志模板
    std::string getLogpointMessage(int line) const;

    /// R104 Function Breakpoint：添加函数断点（按函数名）
    void setFunctionBreakpoint(const std::string& functionName);

    /// R104 Function Breakpoint：移除函数断点
    void removeFunctionBreakpoint(const std::string& functionName);

    /// R104 Function Breakpoint：批量设置函数断点（替换全部）
    void setFunctionBreakpoints(const QSet<std::string>& names);

    /// R104 Function Breakpoint：查询所有函数断点名
    QSet<std::string> getFunctionBreakpoints() const;

    /// R104 Function Breakpoint：设置函数断点条件
    void setFunctionBreakpointCondition(const std::string& functionName, const std::string& condition);

    /// R104 Function Breakpoint：获取函数断点条件
    std::string getFunctionBreakpointCondition(const std::string& functionName) const;

    /// R104 Function Breakpoint：获取函数断点命中次数
    int getFunctionBreakpointHitCount(const std::string& functionName) const;

    /// R104 Function Breakpoint：查询是否存在指定函数断点
    bool hasFunctionBreakpoint(const std::string& functionName) const;

    /// R104 Function Breakpoint：Interpreter 在 callNamedFunction / callClosureValue 入口调用。
    /// 若函数名匹配且条件（可选）满足，则递增 hitCount 并通过 doPause 暂停。
    /// @param functionName 被调用函数名
    /// @param line 调用所在行号（用于 pausedAt 信号）
    /// @return true 表示已暂停（调用方应在调用后立即检查 stopped_ 标志）
    bool checkFunctionBreakpoint(const std::string& functionName, int line);

    /// R104 Exception Breakpoint：启用/禁用异常断点（throw 前暂停）
    void setExceptionBreakpointEnabled(bool enabled);

    /// R104 Exception Breakpoint：查询异常断点是否启用
    bool isExceptionBreakpointEnabled() const;

    /// R104 Exception Breakpoint：获取异常断点命中次数
    int getExceptionBreakpointHitCount() const;

    /// R104 Exception Breakpoint：Interpreter 在 visitThrowStmt 抛出前调用。
    /// 若异常断点启用，则递增 hitCount 并通过 doPause 暂停。
    /// @param line throw 语句所在行号（用于 pausedAt 信号）
    /// @return true 表示已暂停（调用方应在调用后立即检查 stopped_ 标志）
    bool checkExceptionBreakpoint(int line);

    /// 步进控制
    void stepIn();
    void stepOver();
    void stepOut();
    void resume();
    void stop();

    /// R98 runToCursor: 设置一次性临时断点（仅命中一次后自动清除）。
    /// 调用此方法后调用 resume() 即可"运行到目标行"。
    /// 与用户断点独立——临时断点命中后自动清除，不影响用户断点。
    /// 已设置未命中的临时断点会被新调用覆盖（取最后一次目标行）。
    /// @param line 目标行号（必须 > 0，否则忽略）
    /// @note stop()/reset()/析构会清除临时断点
    void setTemporaryBreakpoint(int line);

    /// R98 runToCursor: 清除临时断点（手动取消/停止/重置时调用）。
    /// 线程安全：pauseMutex_ 保护。
    void clearTemporaryBreakpoint();

    /// R98 runToCursor: 查询当前临时断点行号（调试/测试用）。
    /// @return 临时断点行号；无临时断点时返回 -1
    int getTemporaryBreakpoint() const;

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

    /// R104 Logpoint：日志断点命中并完成日志输出后发射。
    /// @param line Logpoint 所在行号
    /// @param message 已格式化的日志消息（{expr} 占位符已求值替换）
    /// @note 本信号在 worker 线程发射（与 pausedAt 同），UI 通过 Qt::QueuedConnection 安全接收
    void logpointLogged(int line, const std::string& message);

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
    // R98 runToCursor: 一次性临时断点。runToCursor 设置后由 checkBreak 慢速路径检测，
    // 命中即清除并暂停。与 breakpoints_ 独立存储避免影响用户断点（BreakpointConditionPanel
    // 显示/编辑不应感知临时断点）。由 pauseMutex_ 保护（worker 线程在 checkBreak 中读取）。
    int tempBreakpointLine_ = -1; // -1 表示无临时断点；>0 为目标行号
    // R98 runToCursor: 原子快速路径标志（与 hasBreakpoints_ 同级），临时断点存在时为 true。
    // checkBreak 快速路径必须同时检查 hasBreakpoints_ 和 hasTempBreakpoint_ 才能无锁返回。
    std::atomic<bool> hasTempBreakpoint_{false};

    // R104 Function Breakpoint：函数名 → 详情。由 pauseMutex_ 保护（与 breakpoints_ 同锁）。
    // Interpreter 在 callNamedFunction / callClosureValue 入口调用 checkFunctionBreakpoint 查询。
    QMap<std::string, FunctionBreakpointInfo> functionBreakpoints_;
    // R104 Exception Breakpoint：单一全局开关。由 pauseMutex_ 保护。
    // Interpreter 在 visitThrowStmt 抛出前调用 checkExceptionBreakpoint 查询。
    ExceptionBreakpointState exceptionBreakpoint_;
    // R104 Logpoint：日志输出回调。锁外调用（与 outputCallback_ 同模式）。
    // 命中 Logpoint 时通过此回调输出格式化后的日志消息。
    std::function<void(const std::string&)> logCallback_;

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

    /// R104 Logpoint：格式化日志消息（{expr} 占位符求值替换）。
    /// 通过 evaluator_ 在沙箱中求值每个 {expr}，结果用 Value::toString() 转字符串后替换。
    /// 占位符语法：{表达式}，如 "i={i}, sum={sum}" 中 {i} 和 {sum} 被求值替换。
    /// 不合法的占位符（解析失败/求值异常）保留原样不替换。
    /// @param templateStr 日志模板
    /// @param line Logpoint 所在行号（用于沙箱求值上下文）
    std::string formatLogpointMessage(const std::string& templateStr, int line);
};
