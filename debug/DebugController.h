#pragma once

#include "common/IDebugController.h" // P1-4: 调试控制器统一接口
#include "debug/DebugEvaluator.h"    // A5 fix: 抽取条件断点求值器
#include "debug/DebugTypes.h"        // ARCH-16 fix: 共享调试公共类型
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
/// P1-4 fix: 继承 IDebugController，统一 DebugController（Interpreter 路径）与
/// VmStepper（VM 路径）的类型契约。步进操作/状态快照/内部 hook 保留在具体类。
class DebugController : public QObject, public IDebugController {
    Q_OBJECT

public:
    explicit DebugController(QObject* parent = nullptr);
    ~DebugController() override;

    /// 在每个 AST 节点执行前调用
    void checkBreak(class ASTNode* node);

    /// 断点管理
    void setBreakpoint(int line);
    void setBreakpoints(const QSet<int>& lines) override;
    void removeBreakpoint(int line);
    void toggleBreakpoint(int line);
    bool hasBreakpoint(int line) const;
    QSet<int> getBreakpoints() const;

    /// 条件断点：设置断点条件表达式
    void setBreakpointCondition(int line, const std::string& condition);

    /// 获取断点条件
    std::string getBreakpointCondition(int line) const;

    /// 获取断点命中次数
    int getBreakpointHitCount(int line) const override;

    // ---- 拓展二期：命中条件（hit condition）+ 依赖断点链 ----
    /// 设置命中条件（"N"/"== N"/">= N"/"> N"/"% N"，空=清除）。
    /// 变更时重置该行 hitCount（对齐 setBreakpointCondition 语义）。
    void setBreakpointHitCondition(int line, const std::string& expr);
    std::string getBreakpointHitCondition(int line) const;

    /// 设置依赖断点链：仅当 depLine 的断点至少命中过一次后，
    /// line 的断点才激活（depLine <= 0 清除依赖）。
    void setBreakpointDependency(int line, int depLine);
    int getBreakpointDependency(int line) const;

    /// 设置条件表达式求值回调（由 IDE 设置，接收条件字符串，返回 bool）
    // A5 fix: 委托给 DebugEvaluator，DebugController 不再直接持有回调
    void setConditionEvaluator(std::function<bool(const std::string&)> evaluator) override;

    /// R104 Logpoint：设置日志输出回调（由 IDE 设置，接收日志字符串）。
    /// Logpoint 命中时调用此回调输出格式化后的日志消息。
    /// 若未设置，Logpoint 命中后日志被丢弃（仍递增 hitCount）。
    void setLogCallback(std::function<void(const std::string&)> cb) override;

    /// R104 Logpoint：设置断点类型（Line / Logpoint）
    void setBreakpointKind(int line, BreakpointKind kind) override;

    /// R104 Logpoint：获取断点类型
    BreakpointKind getBreakpointKind(int line) const override;

    /// R104 Logpoint：设置 Logpoint 日志模板（如 "i={i}, sum={sum}"）。
    /// 模板中的 {expr} 占位符在沙箱中求值后替换为对应的字符串表示。
    void setLogpointMessage(int line, const std::string& msg) override;

    /// R104 Logpoint：获取 Logpoint 日志模板
    std::string getLogpointMessage(int line) const override;

    /// R104 Function Breakpoint：添加函数断点（按函数名）
    void setFunctionBreakpoint(const std::string& functionName) override;

    /// R104 Function Breakpoint：移除函数断点
    void removeFunctionBreakpoint(const std::string& functionName) override;

    /// R104 Function Breakpoint：批量设置函数断点（替换全部）
    void setFunctionBreakpoints(const QSet<std::string>& names) override;

    /// R104 Function Breakpoint：查询所有函数断点名
    QSet<std::string> getFunctionBreakpoints() const override;

    /// R104 Function Breakpoint：设置函数断点条件
    void setFunctionBreakpointCondition(const std::string& functionName, const std::string& condition) override;

    /// R104 Function Breakpoint：获取函数断点条件
    std::string getFunctionBreakpointCondition(const std::string& functionName) const override;

    /// R104 Function Breakpoint：获取函数断点命中次数
    int getFunctionBreakpointHitCount(const std::string& functionName) const override;

    /// R104 Function Breakpoint：查询是否存在指定函数断点
    bool hasFunctionBreakpoint(const std::string& functionName) const override;

    /// R104 Function Breakpoint：Interpreter 在 callNamedFunction / callClosureValue 入口调用。
    /// 若函数名匹配且条件（可选）满足，则递增 hitCount 并通过 doPause 暂停。
    /// @param functionName 被调用函数名
    /// @param line 调用所在行号（用于 pausedAt 信号）
    /// @return true 表示已暂停（调用方应在调用后立即检查 stopped_ 标志）
    bool checkFunctionBreakpoint(const std::string& functionName, int line);

    /// R104 Exception Breakpoint：启用/禁用异常断点（throw 前暂停）
    void setExceptionBreakpointEnabled(bool enabled) override;

    /// R104 Exception Breakpoint：查询异常断点是否启用
    bool isExceptionBreakpointEnabled() const override;

    /// R104 Exception Breakpoint：获取异常断点命中次数
    int getExceptionBreakpointHitCount() const override;

    /// R104 Exception Breakpoint：Interpreter 在 visitThrowStmt 抛出前调用。
    /// 若异常断点启用，则递增 hitCount 并通过 doPause 暂停。
    /// @param line throw 语句所在行号（用于 pausedAt 信号）
    /// @return true 表示已暂停（调用方应在调用后立即检查 stopped_ 标志）
    bool checkExceptionBreakpoint(int line);

    /// L19 Watchpoint（Interpreter 路径）：添加数据断点。
    /// Interpreter 路径的 watchpoint 存储在 DebugController（与 VmStepper 的 vmWatchpoints_ 独立），
    /// 由 visitAssignment/visitVarDecl/visitMemberAssign/visitIndexAssign 入口检查。
    void setWatchpoint(const WatchpointInfo& wp) override;

    /// L19 Watchpoint（Interpreter 路径）：移除匹配的数据断点。
    /// varName 非空时按变量名匹配；fieldName 非空时按字段名匹配（Field 类型）。
    void removeWatchpoint(const std::string& varName, const std::string& fieldName = "") override;

    /// L19 Watchpoint（Interpreter 路径）：清空所有数据断点
    void clearWatchpoints() override;

    /// L19 Watchpoint（Interpreter 路径）：查询所有数据断点（值拷贝，锁保护）
    std::vector<WatchpointInfo> getWatchpoints() const;

    /// L19 Watchpoint（Interpreter 路径）：快速路径判断是否存在数据断点。
    /// 原子标志，无锁读取，visit* 方法入口用此短路避免加锁开销。
    bool hasWatchpoints() const override { return hasWatchpoints_.load(std::memory_order_relaxed); }

    /// L19 Watchpoint（Interpreter 路径）：Interpreter 在 visit* 入口调用。
    /// 按 varName/fieldName 匹配 watchpoint，命中时递增 hitCount 并通过 doPause 暂停。
    /// @param varName 写入的变量名（索引写入时为根变量名）
    /// @param isFieldWrite 是否字段写入（visitMemberAssign 为 true，其他为 false）
    /// @param fieldName 字段名（仅 isFieldWrite=true 时有效）
    /// @param line 写入语句所在行号（用于 pausedAt 信号）
    /// @return true 表示已暂停（调用方应在调用后立即检查 stopped_ 标志）
    bool checkWatchpointHit(const std::string& varName, bool isFieldWrite, const std::string& fieldName, int line);

    /// 步进控制
    void stepIn();
    void stepOver();
    void stepOut();
    void resume();
    void stop() override;

    /// R98 runToCursor: 设置一次性临时断点（仅命中一次后自动清除）。
    /// 调用此方法后调用 resume() 即可"运行到目标行"。
    /// 与用户断点独立——临时断点命中后自动清除，不影响用户断点。
    /// 已设置未命中的临时断点会被新调用覆盖（取最后一次目标行）。
    /// @param line 目标行号（必须 > 0，否则忽略）
    /// @note stop()/reset()/析构会清除临时断点
    void setTemporaryBreakpoint(int line) override;

    /// R98 runToCursor: 清除临时断点（手动取消/停止/重置时调用）。
    /// 线程安全：pauseMutex_ 保护。
    void clearTemporaryBreakpoint() override;

    /// R98 runToCursor: 查询当前临时断点行号（调试/测试用）。
    /// @return 临时断点行号；无临时断点时返回 -1
    int getTemporaryBreakpoint() const override;

    /// 设置当前调用深度（由 Interpreter 更新）
    void setCurrentDepth(int depth);

    /// 设置变量快照回调（由主窗口设置）
    void setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb);

    /// 拓展二期：变量写回调（由 DebugCoordinator 注册，内部调用
    /// Interpreter 当前 Environment::set 沿作用域链写入）。
    void setVariableWriteCallback(std::function<bool(const std::string&, const Value&)> cb);

    /// 拓展二期：调试暂停时写变量（setVariable）。
    /// 仅 isPaused() 时有效（worker 阻塞在 pauseCV_，GUI 线程写 Environment
    /// 是安全窗口）。@return true 写入成功；false 未暂停/变量不存在/无回调
    bool setVariableValue(const std::string& name, const Value& value);

    /// 设置调用栈回调
    void setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb);

    /// 获取变量快照
    std::vector<VariableSnapshot> getVariableSnapshot() const;

    /// 获取调用栈
    std::vector<CallStackEntry> getCallStack() const override;

    /// 是否正在运行
    bool isRunning() const;

    /// 是否处于暂停状态
    bool isPaused() const;

    /// 重置状态
    void reset() override;

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
    std::atomic<int> currentDepth_{0}; // 当前调用深度（worker 写，UI 读）
    // AUDIT-R4 BUG-16 fix: stepOverDepth_/stepOutDepth_/tempBreakpointLine_ 改为
    // atomic<int>——reset() 在 terminate 防御路径用 try_lock，获锁失败时原实现
    // 跳过这些非原子字段的重置，残留旧值使下次调试会话首次单步行为异常。
    // atomic 化后 reset() 可无条件重置；复合更新仍在 pauseMutex_ 内进行
    // （atomic 在锁内读写合法），不改变既有同步语义。
    std::atomic<int> stepOverDepth_{0};       // stepOver 时的调用深度
    std::atomic<int> stepOutDepth_{0};        // stepOut 时的调用深度
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
    // AUDIT-R4 BUG-16 fix: 同 stepOverDepth_，atomic 化使 reset() 获锁失败时仍可重置。
    std::atomic<int> tempBreakpointLine_{-1}; // -1 表示无临时断点；>0 为目标行号
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

    // L19 Watchpoint（Interpreter 路径）：数据断点列表。由 pauseMutex_ 保护。
    // Interpreter 在 visitAssignment/visitVarDecl/visitMemberAssign/visitIndexAssign 入口
    // 调用 checkWatchpointHit 查询。与 VmStepper::vmWatchpoints_ 独立（VM 路径用 peekWriteTarget）。
    std::vector<WatchpointInfo> watchpoints_;
    // L19 Watchpoint：原子快速路径标志（与 hasBreakpoints_ 同级）。
    // visit* 方法入口通过 hasWatchpoints() 无锁判断，避免每次赋值都加锁。
    std::atomic<bool> hasWatchpoints_{false};

    // A2: 线程安全的暂停/恢复机制（替代 QEventLoop）
    mutable std::mutex pauseMutex_; // P0-9 fix: mutable 以便 const 方法加锁
    std::condition_variable pauseCV_;

    std::function<std::vector<VariableSnapshot>()> variableCallback_;
    std::function<std::vector<CallStackEntry>()> callStackCallback_;
    // 拓展二期：变量写回调（setVariableValue 用，与 variableCallback_ 同锁保护）
    std::function<bool(const std::string&, const Value&)> variableWriteCallback_;

    // AUDIT-P1 fix: 活跃 callback 计数，用于析构时等待正在执行的 callback 完成（RCU 优雅期模式）。
    // getVariableSnapshot/getCallStack 在锁外调用 cb() 期间增减此计数，
    // DebugCoordinator 析构清空 callback 后 spin-wait 直到计数归零，避免 UAF。
    // P0-2 fix: 改为 shared_ptr<atomic>，生命周期独立于 DebugController。CountGuard 持有
    // 副本，waitCallbacksIdle 超时后继续析构也不会 UAF（worker 的 CountGuard 保持 atomic 存活）。
    mutable std::shared_ptr<std::atomic<int>> activeCallbackCount_{std::make_shared<std::atomic<int>>(0)};

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
