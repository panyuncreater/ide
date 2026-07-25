#include "debug/DebugController.h"
#include "ast/ASTNode.h"
#include "common/Logger.h"
#include "interpreter/RuntimeExceptions.h" // S6 fix: DebugStopException 定义
#include <chrono>                          // AUDIT-P2-CORRECT fix: waitCallbacksIdle 超时
#include <stdexcept>
#include <thread> // AUDIT-P1 fix: std::this_thread::yield for waitCallbacksIdle

// ============================================================
// DebugController 调试控制器实现
// ============================================================

DebugController::DebugController(QObject* parent) : QObject(parent) {}

DebugController::~DebugController() {
    stopped_ = true;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        paused_ = false;
        mode_.store(static_cast<int>(StepMode::MODE_RUN));
        // R98 runToCursor: 析构时清除临时断点，防止 worker 线程在 ~DebugController
        // 与 waitCallbacksIdle 之间的窗口内仍读取已失效的 tempBreakpointLine_。
        tempBreakpointLine_ = -1;
        hasTempBreakpoint_.store(false);
    }
    pauseCV_.notify_all();
    // AUDIT-P2-CORRECT fix: 防御性等待所有锁外 callback 完成，避免析构期间
    // worker 线程仍在执行 getVariableSnapshot/getCallStack 的锁外 callback 导致 UAF。
    // 对齐 DebugEvaluator 析构调用 waitCallbackIdle 的模式。
    waitCallbacksIdle();
}

void DebugController::checkBreak(ASTNode* node) {
    if (!node)
        return;

    // 如果已被停止，立即终止执行（优先级最高）
    if (stopped_) {
        throw DebugStopException();
    }

    if (!running_)
        return;

    // D-P1-1 fix: 原子快速路径——RUN 模式且无断点（含 R98 临时断点）时，无锁返回
    // R98 runToCursor: 必须同时检查 hasTempBreakpoint_，否则临时断点会被无锁跳过
    if (static_cast<StepMode>(mode_.load()) == StepMode::MODE_RUN && !hasBreakpoints_.load() &&
        !hasTempBreakpoint_.load()) {
        // PERF-ROUND53 fix: 移除快速路径的 updateLineTracking 调用。
        // 原实现每节点执行 2 次原子 load + 1-2 次原子 store（crossedLine_/lastSeenLine_），
        // 在紧密循环百万级节点中累积可观开销。快速路径前提是无断点，此时
        // crossedLine_/lastSeenLine_ 不被任何暂停逻辑读取。用户添加断点后
        // hasBreakpoints_ 变 true 进入慢速路径，updateLineTracking（L85）会重新计算
        // crossedLine_（line != lastSeenLine_，lastSeenLine_ 为旧值时为 true），
        // 不影响正确性。回退 D-P2-11 fix。
        return;
    }

    // Thread-safety: take a snapshot of shared state under the mutex.
    StepMode snapMode;
    QSet<int> localBreakpoints;
    QMap<int, BreakpointInfo> localBreakpointInfos;
    int snapCurrentDepth, snapStepOverDepth, snapStepOutDepth;
    int snapLastPausedLine, snapLastPausedDepth, snapMinBreakpointLine;
    bool snapCrossedDeeper;
    int snapTempBreakpointLine; // R98 runToCursor: 临时断点行号快照
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        snapMode = static_cast<StepMode>(mode_.load());
        localBreakpoints = breakpoints_;
        localBreakpointInfos = breakpointInfos_;
        snapCurrentDepth = currentDepth_.load();
        snapStepOverDepth = stepOverDepth_;
        snapStepOutDepth = stepOutDepth_;
        snapLastPausedLine = lastPausedLine_.load();
        snapLastPausedDepth = lastPausedDepth_.load();
        snapMinBreakpointLine = minBreakpointLine_;
        snapCrossedDeeper = crossedDeeper_.load();
        snapTempBreakpointLine = tempBreakpointLine_;
    }

    // 慢速路径中的二次确认（hasBreakpoints_ 原子读可能与锁内状态有微小窗口）
    // R98 runToCursor: 必须同时考虑临时断点存在性
    if (snapMode == StepMode::MODE_RUN && localBreakpoints.empty() && snapTempBreakpointLine < 0) {
        updateLineTracking(node->line, 0, 0, StepMode::MODE_RUN);
        return;
    }

    // DBG-B fix: Step Over 模式下追踪是否进入了更深的调用层
    if (snapMode == StepMode::MODE_STEP_OVER && snapCurrentDepth > snapStepOverDepth) {
        crossedDeeper_.store(true);
        snapCrossedDeeper = true;
    }

    // AUDIT-P1 fix: updateLineTracking 必须在 shouldPause 之前执行，否则
    // shouldPauseAtBreakpoint 命中时设 crossedLine_=false 会被随后的
    // updateLineTracking 覆盖为 true（line != lastSeenLine_），导致 resume 后
    // 同行下一个 AST 子表达式断点重复触发。
    updateLineTracking(node->line, snapCurrentDepth, snapStepOverDepth, snapMode);

    // R98 runToCursor: 临时断点检测——优先于用户断点和步进逻辑判断。
    // 仅在 MODE_RUN 下检测（与 setTemporaryBreakpoint 后调用 resume() 的预期一致）；
    // 不要求 crossedLine_（临时断点首次到达即命中，无需"跨行后重触发"语义，因为
    // 它是一次性的）。命中后立即清除临时断点字段，防止再次触发。
    if (snapMode == StepMode::MODE_RUN && snapTempBreakpointLine > 0 && node->line == snapTempBreakpointLine) {
        {
            std::lock_guard<std::mutex> lock(pauseMutex_);
            tempBreakpointLine_ = -1;
            hasTempBreakpoint_.store(false);
        }
        // 临时断点命中视为断点事件，重置 crossedLine_ 避免同行后续子表达式误触发用户断点
        crossedLine_.store(false);
        doPause(node->line, snapCurrentDepth);
        return;
    }

    // C11 fix: 委托给助手方法判断是否应暂停
    bool shouldPause = false;
    if (snapMode == StepMode::MODE_RUN) {
        shouldPause =
            shouldPauseAtBreakpoint(node->line, localBreakpoints, localBreakpointInfos, snapMinBreakpointLine);
    } else {
        shouldPause =
            shouldPauseForStepping(snapMode, node->line, snapCurrentDepth, snapStepOverDepth, snapStepOutDepth,
                                   snapLastPausedLine, snapLastPausedDepth, snapCrossedDeeper);
    }

    // R54-8 fix: 步进模式下仅在真正暂停时递增 hitCount，与 VmStepper（L226-228
    // 仅在断点命中路径递增）和 RUN 模式（shouldPauseAtBreakpoint L140/153 仅在
    // 命中时递增）对齐。原实现（M10 + DBG-04 fix）在经过断点行时即递增，不区分
    // 是否暂停，导致 STEP_OVER 进入更深帧期间经过断点行时 hitCount 被反复刷高，
    // BreakpointConditionPanel 显示的命中次数远超实际暂停次数。
    if (snapMode != StepMode::MODE_RUN && shouldPause && node->line > 0) {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        auto realIt = breakpointInfos_.find(node->line);
        if (realIt != breakpointInfos_.end()) {
            realIt->hitCount++;
        }
    }

    if (shouldPause && node->line > 0) {
        doPause(node->line, snapCurrentDepth);
    }
}

// C11 fix: checkBreak 助手方法实现

bool DebugController::shouldPauseAtBreakpoint(int line, const QSet<int>& localBreakpoints,
                                              const QMap<int, BreakpointInfo>& localBreakpointInfos,
                                              int snapMinBreakpointLine) {
    // C3 fix + DBG-03: 单行循环断点重触发——用 crossedLine_ 检测是否跨过不同行
    if ((line != lastSeenLine_.load() || crossedLine_.load()) && line >= snapMinBreakpointLine &&
        localBreakpoints.contains(line)) {
        // AUDIT-BUG-F4 fix: crossedLine_ 重置必须推迟到断点真正命中（return true）之前。
        // 原实现在条件求值之前重置，条件不满足时 crossedLine_ 已被清 false，
        // 单行循环中行号不变 → crossedLine_ 永不再变 true → 条件断点首次不满足后永不再触发。
        // 检查是否为条件断点
        auto infoIt = localBreakpointInfos.find(line);
        // R104 Logpoint：命中不暂停，仅输出日志并递增 hitCount
        // Logpoint 仍可附加条件（条件为真时才输出日志）
        if (infoIt != localBreakpointInfos.end() && infoIt->isLogpoint()) {
            bool shouldLog = true;
            if (infoIt->isConditional()) {
                if (evaluator_ && evaluator_->hasCallback()) {
                    shouldLog = evaluator_->evaluate(infoIt->condition, line);
                } else {
                    shouldLog = false; // 无求值器，条件 Logpoint 视为不命中
                }
            }
            if (shouldLog) {
                // 递增 hitCount（锁内）
                {
                    std::lock_guard<std::mutex> lock(pauseMutex_);
                    auto realIt = breakpointInfos_.find(line);
                    if (realIt != breakpointInfos_.end())
                        realIt->hitCount++;
                }
                // 格式化日志消息并输出（锁外，避免持锁回调死锁）
                std::string logMsg = formatLogpointMessage(infoIt->logMessage, line);
                std::function<void(const std::string&)> cb;
                {
                    std::lock_guard<std::mutex> lock(pauseMutex_);
                    cb = logCallback_;
                }
                if (cb) {
                    try {
                        cb(logMsg);
                    } catch (...) {
                        // 日志回调异常被吞掉，避免影响程序执行
                    }
                }
                // 发射 logpointLogged 信号（UI 通过 Qt::QueuedConnection 接收）
                emit logpointLogged(line, logMsg);
                // Logpoint 命中后重置 crossedLine_，同行后续子表达式不再触发
                crossedLine_.store(false);
            }
            // Logpoint 永远不暂停
            return false;
        }
        if (infoIt != localBreakpointInfos.end() && infoIt->isConditional()) {
            // A5 fix: 委托给 DebugEvaluator 求值（异常处理 + 日志已封装）
            if (evaluator_ && evaluator_->hasCallback()) {
                if (evaluator_->evaluate(infoIt->condition, line)) {
                    {
                        std::lock_guard<std::mutex> lock(pauseMutex_);
                        auto realIt = breakpointInfos_.find(line);
                        if (realIt != breakpointInfos_.end())
                            realIt->hitCount++;
                    }
                    crossedLine_.store(false); // 命中后重置，同行后续子表达式不再触发
                    return true;
                }
                // 条件不满足：保留 crossedLine_ 状态，允许下次迭代重新求值
            }
        } else {
            // 无条件断点：直接暂停
            if (infoIt != localBreakpointInfos.end()) {
                std::lock_guard<std::mutex> lock(pauseMutex_);
                auto realIt = breakpointInfos_.find(line);
                if (realIt != breakpointInfos_.end())
                    realIt->hitCount++;
            }
            crossedLine_.store(false); // 命中后重置，同行后续子表达式不再触发
            return true;
        }
    }
    return false;
}

bool DebugController::shouldPauseForStepping(StepMode snapMode, int line, int snapCurrentDepth, int snapStepOverDepth,
                                             int snapStepOutDepth, int snapLastPausedLine, int snapLastPausedDepth,
                                             bool snapCrossedDeeper) {
    (void)line; // STEP_OUT 不使用 line
    switch (snapMode) {
    case StepMode::MODE_STEP_IN:
        // 行号变化时暂停，或调用深度变化时暂停（递归函数同行不同深度）
        return (line != snapLastPausedLine || snapCurrentDepth != snapLastPausedDepth);
    case StepMode::MODE_STEP_OVER:
        // 只暂停同一或更浅调用深度，且行号变化或从深层返回
        if (snapCurrentDepth <= snapStepOverDepth && (line != snapLastPausedLine || snapCrossedDeeper)) {
            return true;
        }
        return false;
    case StepMode::MODE_STEP_OUT:
        // H6 fix: 仅检查调用深度，深度变浅即已从函数返回
        return (snapCurrentDepth < snapStepOutDepth);
    default:
        return false;
    }
}

void DebugController::updateLineTracking(int line, int snapCurrentDepth, int snapStepOverDepth, StepMode snapMode) {
    (void)snapCurrentDepth;
    (void)snapStepOverDepth;
    (void)snapMode;
    if (line > 0) {
        if (line != lastSeenLine_.load())
            crossedLine_.store(true);
        lastSeenLine_.store(line);
    }
}

void DebugController::doPause(int line, int snapCurrentDepth) {
    lastPausedLine_.store(line);
    lastPausedDepth_.store(snapCurrentDepth);
    crossedDeeper_.store(false); // DBG-B fix: 暂停后重置

    // Perf-LazyLog: LOG_DEBUG 宏级别过滤后跳过字符串构造（断点命中频繁时收益明显）
    LOG_DEBUG("断点暂停于行 " + std::to_string(line) + " (深度 " + std::to_string(snapCurrentDepth) + ")", "Debugger");

    // V-P0-1/D-P0-1 fix: 锁内原子性地检查 stopped_ 并设置 paused_=true
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (stopped_) {
            throw DebugStopException();
        }
        paused_ = true;
    }
    emit pausedAt(line);
    pauseExecution();

    // D-P2-1 fix: pauseExecution 返回后立即检查 stopped_
    if (stopped_)
        throw DebugStopException();
}

void DebugController::setBreakpoint(int line) {
    // D-P2-6 fix: 校验行号有效性（负数或 0 无意义）
    if (line <= 0)
        return;
    // P0-9 fix: 加锁保护容器（checkBreak 在 worker 线程读取）
    std::lock_guard<std::mutex> lock(pauseMutex_);
    breakpoints_.insert(line);
    // 若尚无 BreakpointInfo 条目则创建（保留已有条件）
    if (!breakpointInfos_.contains(line)) {
        breakpointInfos_.insert(line, BreakpointInfo(line));
    }
    updateMinBreakpointLine();
    // D-P1-1 fix: 更新原子标志位
    hasBreakpoints_.store(!breakpoints_.empty());
}

void DebugController::setBreakpoints(const QSet<int>& lines) {
    // P0-9 fix: 加锁保护容器
    std::lock_guard<std::mutex> lock(pauseMutex_);
    // D-P2-6 fix: 过滤无效行号
    QSet<int> validLines;
    for (int line : lines) {
        if (line > 0)
            validLines.insert(line);
    }
    breakpoints_ = validLines;
    // 同步 breakpointInfos_：移除不再存在的，添加新增的
    // P0-10 fix: 不再重置已有断点的 hitCount（保留累计命中次数）
    auto it = breakpointInfos_.begin();
    while (it != breakpointInfos_.end()) {
        if (!validLines.contains(it.key())) {
            it = breakpointInfos_.erase(it);
        } else {
            ++it;
        }
    }
    for (int line : validLines) {
        if (!breakpointInfos_.contains(line)) {
            breakpointInfos_.insert(line, BreakpointInfo(line));
        }
    }
    updateMinBreakpointLine();
    // D-P1-1 fix: 更新原子标志位
    hasBreakpoints_.store(!breakpoints_.empty());
}

void DebugController::removeBreakpoint(int line) {
    // P0-9 fix: 加锁保护容器
    std::lock_guard<std::mutex> lock(pauseMutex_);
    breakpoints_.remove(line);
    breakpointInfos_.remove(line);
    updateMinBreakpointLine();
    // D-P1-1 fix: 更新原子标志位
    hasBreakpoints_.store(!breakpoints_.empty());
}

void DebugController::toggleBreakpoint(int line) {
    // D-P2-6 fix: 校验行号有效性
    if (line <= 0)
        return;
    // P0-9 fix: 加锁保护容器
    std::lock_guard<std::mutex> lock(pauseMutex_);
    if (breakpoints_.contains(line)) {
        breakpoints_.remove(line);
        // DBG-07 fix: 保留 breakpointInfos_ 中的条件表达式，仅移除活跃断点
    } else {
        breakpoints_.insert(line);
        if (!breakpointInfos_.contains(line)) {
            breakpointInfos_.insert(line, BreakpointInfo(line));
        }
    }
    updateMinBreakpointLine();
    // D-P1-1 fix: 更新原子标志位
    hasBreakpoints_.store(!breakpoints_.empty());
}

bool DebugController::hasBreakpoint(int line) const {
    // P0-9 fix: 加锁保护容器读取
    std::lock_guard<std::mutex> lock(pauseMutex_);
    return breakpoints_.contains(line);
}

QSet<int> DebugController::getBreakpoints() const {
    // P0-9 fix: 加锁保护容器读取
    std::lock_guard<std::mutex> lock(pauseMutex_);
    return breakpoints_;
}

void DebugController::setBreakpointCondition(int line, const std::string& condition) {
    // D-P2-6 fix: 校验行号有效性
    if (line <= 0)
        return;
    // P0-9 fix: 加锁保护容器
    std::lock_guard<std::mutex> lock(pauseMutex_);
    // 确保断点存在
    if (!breakpoints_.contains(line)) {
        breakpoints_.insert(line);
        if (!breakpointInfos_.contains(line)) {
            breakpointInfos_.insert(line, BreakpointInfo(line));
        }
        updateMinBreakpointLine();
        // D-P1-1 fix: 更新原子标志位
        hasBreakpoints_.store(!breakpoints_.empty());
    }
    // 更新或创建 BreakpointInfo
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        it->condition = condition;
        it->hitCount = 0; // 条件变更时重置命中计数
    } else {
        breakpointInfos_.insert(line, BreakpointInfo(line, condition));
    }
}

std::string DebugController::getBreakpointCondition(int line) const {
    // P0-9 fix: 加锁保护容器读取
    std::lock_guard<std::mutex> lock(pauseMutex_);
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        return it->condition;
    }
    return "";
}

int DebugController::getBreakpointHitCount(int line) const {
    // P0-9 fix: 加锁保护容器读取
    std::lock_guard<std::mutex> lock(pauseMutex_);
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        return it->hitCount;
    }
    return 0;
}

void DebugController::setConditionEvaluator(std::function<bool(const std::string&)> evaluator) {
    // A5 fix: 委托给 DebugEvaluator（内部自带 mutex 保护）
    evaluator_->setCallback(std::move(evaluator));
}

// ============================================================
// R104 调试器拓展：Logpoint / Function Breakpoint / Exception Breakpoint
// ============================================================
//
// 设计原则：
// - Logpoint 复用 BreakpointInfo（同为行锚点），通过 kind 字段区分
// - Function/Exception Breakpoint 独立存储（不依赖行号）
// - 三类断点共享 doPause() 暂停机制与 pausedAt 信号
// - 线程安全：所有存储由 pauseMutex_ 保护（与现有断点同锁）
// ============================================================

void DebugController::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    logCallback_ = std::move(cb);
}

void DebugController::setBreakpointKind(int line, BreakpointKind kind) {
    if (line <= 0)
        return;
    std::lock_guard<std::mutex> lock(pauseMutex_);
    // 确保断点存在
    if (!breakpoints_.contains(line)) {
        breakpoints_.insert(line);
        if (!breakpointInfos_.contains(line)) {
            breakpointInfos_.insert(line, BreakpointInfo(line));
        }
        updateMinBreakpointLine();
        hasBreakpoints_.store(!breakpoints_.empty());
    }
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        it->kind = kind;
        // 切换为 Line 时清空 logMessage（避免残留）
        if (kind == BreakpointKind::Line) {
            it->logMessage.clear();
        }
        it->hitCount = 0; // 类型变更时重置命中计数
    }
}

BreakpointKind DebugController::getBreakpointKind(int line) const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        return it->kind;
    }
    return BreakpointKind::Line;
}

void DebugController::setLogpointMessage(int line, const std::string& msg) {
    if (line <= 0)
        return;
    std::lock_guard<std::mutex> lock(pauseMutex_);
    if (!breakpoints_.contains(line)) {
        breakpoints_.insert(line);
        if (!breakpointInfos_.contains(line)) {
            breakpointInfos_.insert(line, BreakpointInfo(line));
        }
        updateMinBreakpointLine();
        hasBreakpoints_.store(!breakpoints_.empty());
    }
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        it->kind = BreakpointKind::Logpoint;
        it->logMessage = msg;
        it->hitCount = 0; // 消息变更时重置命中计数
    }
}

std::string DebugController::getLogpointMessage(int line) const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        return it->logMessage;
    }
    return "";
}

std::string DebugController::formatLogpointMessage(const std::string& templateStr, int line) {
    // R104 v1 实现：直接返回模板字符串，不进行 {expr} 插值。
    // {expr} 插值需要新增返回 string 的求值器回调（与现有 bool 条件求值器独立），
    // 涉及 Interpreter::evaluateExpressionString 新 API + 沙箱求值扩展。
    // v1 提供"非暂停日志断点"核心价值：用户可设置带条件的 Logpoint，
    // 条件为真时输出固定消息，追踪程序执行流而不暂停。
    // 后续可拓展 {expr} 插值：扫描模板中的 {...} 模式，对每个 expr 调用
    // logpointEvaluator_(expr) 获取字符串表示并替换。
    (void)line; // 预留：未来插值需要行号上下文
    return templateStr;
}

void DebugController::setFunctionBreakpoint(const std::string& functionName) {
    if (functionName.empty())
        return;
    std::lock_guard<std::mutex> lock(pauseMutex_);
    if (!functionBreakpoints_.contains(functionName)) {
        functionBreakpoints_.insert(functionName, FunctionBreakpointInfo(functionName));
    }
}

void DebugController::removeFunctionBreakpoint(const std::string& functionName) {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    functionBreakpoints_.remove(functionName);
}

void DebugController::setFunctionBreakpoints(const QSet<std::string>& names) {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    // 保留已有命中次数（对齐 setBreakpoints 的 P0-10 fix 语义）
    QMap<std::string, FunctionBreakpointInfo> newMap;
    for (const auto& name : names) {
        if (name.empty())
            continue;
        auto it = functionBreakpoints_.find(name);
        if (it != functionBreakpoints_.end()) {
            newMap.insert(name, it.value());
        } else {
            newMap.insert(name, FunctionBreakpointInfo(name));
        }
    }
    functionBreakpoints_ = std::move(newMap);
}

QSet<std::string> DebugController::getFunctionBreakpoints() const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    QSet<std::string> result;
    for (auto it = functionBreakpoints_.begin(); it != functionBreakpoints_.end(); ++it) {
        result.insert(it.key());
    }
    return result;
}

void DebugController::setFunctionBreakpointCondition(const std::string& functionName, const std::string& condition) {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    auto it = functionBreakpoints_.find(functionName);
    if (it != functionBreakpoints_.end()) {
        it->condition = condition;
        it->hitCount = 0; // 条件变更时重置命中计数（对齐 setBreakpointCondition 语义）
    } else {
        // 函数断点不存在时自动创建（对齐 setBreakpointCondition 行为）
        FunctionBreakpointInfo info(functionName);
        info.condition = condition;
        functionBreakpoints_.insert(functionName, std::move(info));
    }
}

std::string DebugController::getFunctionBreakpointCondition(const std::string& functionName) const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    auto it = functionBreakpoints_.find(functionName);
    if (it != functionBreakpoints_.end()) {
        return it->condition;
    }
    return "";
}

int DebugController::getFunctionBreakpointHitCount(const std::string& functionName) const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    auto it = functionBreakpoints_.find(functionName);
    if (it != functionBreakpoints_.end()) {
        return it->hitCount;
    }
    return 0;
}

bool DebugController::hasFunctionBreakpoint(const std::string& functionName) const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    return functionBreakpoints_.contains(functionName);
}

bool DebugController::checkFunctionBreakpoint(const std::string& functionName, int line) {
    // 快速路径：无函数断点时立即返回（避免每次函数调用都加锁）
    // 注：functionBreakpoints_ 由 pauseMutex_ 保护，但读取 emptiness 需要锁。
    // 此处先取锁内快照判断，避免漏检。
    FunctionBreakpointInfo snapshot;
    bool matched = false;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (!running_ || stopped_)
            return false;
        auto it = functionBreakpoints_.find(functionName);
        if (it == functionBreakpoints_.end())
            return false;
        snapshot = it.value();
        matched = true;
    }

    // 条件求值（锁外，避免持锁回调死锁）
    if (snapshot.isConditional()) {
        if (evaluator_ && evaluator_->hasCallback()) {
            if (!evaluator_->evaluate(snapshot.condition, line)) {
                return false; // 条件不满足
            }
        } else {
            return false; // 无求值器，条件断点视为不命中
        }
    }

    // 命中：递增 hitCount 并暂停
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        auto it = functionBreakpoints_.find(functionName);
        if (it != functionBreakpoints_.end()) {
            it->hitCount++;
        }
    }
    doPause(line, currentDepth_.load());
    return true;
}

void DebugController::setExceptionBreakpointEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    exceptionBreakpoint_.enabled = enabled;
}

bool DebugController::isExceptionBreakpointEnabled() const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    return exceptionBreakpoint_.enabled;
}

int DebugController::getExceptionBreakpointHitCount() const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    return exceptionBreakpoint_.hitCount;
}

bool DebugController::checkExceptionBreakpoint(int line) {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (!running_ || stopped_)
            return false;
        enabled = exceptionBreakpoint_.enabled;
    }
    if (!enabled)
        return false;

    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        exceptionBreakpoint_.hitCount++;
    }
    doPause(line, currentDepth_.load());
    return true;
}

// L19 Watchpoint（Interpreter 路径）：参照 checkFunctionBreakpoint 模板实现。
// watchpoints_ 由 pauseMutex_ 保护，hasWatchpoints_ 原子标志供快速路径短路。
void DebugController::setWatchpoint(const WatchpointInfo& wp) {
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        watchpoints_.push_back(wp);
    }
    hasWatchpoints_.store(true, std::memory_order_relaxed);
}

void DebugController::removeWatchpoint(const std::string& varName, const std::string& fieldName) {
    bool empty = false;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        // 倒序遍历移除所有匹配项（与 VmStepper::removeWatchpoint 行为一致）。
        // 精确匹配 varName：空 varName 只移除 varName 也为空的 watchpoint（通配 Field watchpoint），
        // 不作为"移除全部"的快捷方式——移除全部应使用 clearWatchpoints()。
        // L19 audit fix: 原实现 varName.empty() || wp.varName == varName 导致空 varName 通配所有，
        // 与 VmStepper 的精确匹配语义不一致，路径切换后 watchpoint 列表不同步。
        for (int i = static_cast<int>(watchpoints_.size()) - 1; i >= 0; --i) {
            const auto& wp = watchpoints_[i];
            bool matchVar = (wp.varName == varName);
            bool matchField = fieldName.empty() || wp.fieldName == fieldName;
            if (matchVar && matchField) {
                watchpoints_.erase(watchpoints_.begin() + i);
            }
        }
        empty = watchpoints_.empty();
    }
    hasWatchpoints_.store(!empty, std::memory_order_relaxed);
}

void DebugController::clearWatchpoints() {
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        watchpoints_.clear();
    }
    hasWatchpoints_.store(false, std::memory_order_relaxed);
}

std::vector<WatchpointInfo> DebugController::getWatchpoints() const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    return watchpoints_;
}

bool DebugController::checkWatchpointHit(const std::string& varName, bool isFieldWrite, const std::string& fieldName,
                                         int line) {
    // 快速路径：无 watchpoint 时立即返回（避免每次赋值都加锁）
    if (!hasWatchpoints_.load(std::memory_order_relaxed))
        return false;

    // 与 VmStepper::checkWatchpointHit 匹配语义对齐：
    // - Variable 类型：varName 完全匹配（索引写入视为修改变量本身，也匹配）
    // - Field 类型：要求 isFieldWrite，fieldName 匹配，varName 空时通配
    WatchpointInfo snapshot;
    bool matched = false;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (!running_ || stopped_)
            return false;
        for (const auto& wp : watchpoints_) {
            if (wp.kind == WatchpointTargetKind::Variable && !isFieldWrite) {
                // L19 audit fix: 添加 !varName.empty() 防御性保护（与 VmStepper 对齐），
                // 避免空 varName 误匹配 varName="" 的 Variable watchpoint。
                if (!varName.empty() && wp.varName == varName) {
                    snapshot = wp;
                    matched = true;
                    break;
                }
            } else if (wp.kind == WatchpointTargetKind::Field && isFieldWrite) {
                if (wp.fieldName == fieldName && (wp.varName.empty() || wp.varName == varName)) {
                    snapshot = wp;
                    matched = true;
                    break;
                }
            }
        }
    }

    if (!matched)
        return false;

    // 条件求值（锁外，避免持锁回调死锁）
    if (snapshot.isConditional()) {
        if (evaluator_ && evaluator_->hasCallback()) {
            if (!evaluator_->evaluate(snapshot.condition, line)) {
                return false; // 条件不满足
            }
        } else {
            return false; // 无求值器，条件 watchpoint 视为不命中
        }
    }

    // 命中：递增 hitCount 并暂停（重新查找避免锁外快照过期）
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        for (auto& wp : watchpoints_) {
            if (wp.kind == snapshot.kind && wp.varName == snapshot.varName && wp.fieldName == snapshot.fieldName &&
                wp.condition == snapshot.condition) {
                wp.hitCount++;
                break;
            }
        }
    }
    doPause(line, currentDepth_.load());
    return true;
}

void DebugController::stepIn() {
    LOG_DEBUG("Step In", "Debugger");
    // P1-8 fix: 消除 TOCTOU。原实现先无锁检查 !running_ 决定走快速路径（无 CV notify），
    // 再加锁走慢速路径。检查与加锁之间存在窗口，且快速路径不 notify 在 worker
    // 极端时序下可能丢唤醒。改为统一加锁路径，notify 对无 waiter 是 no-op。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        // AUDIT-P2 fix: 不清除 stopped_。若 stop() 已设置 stopped_=true，step 应无效
        // （worker 将在 checkBreak 抛 DebugStopException 终止）。原实现清除 stopped_=false
        // 可在 worker 响应 stop 的窗口内取消正在进行的 stop，导致状态机不一致。
        // stopped_ 的清除应由 reset()（新调试会话）负责。
        if (stopped_)
            return;
        mode_.store(static_cast<int>(StepMode::MODE_STEP_IN));
        running_ = true;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::stepOver() {
    // P2-5 fix: 在锁内统一读取 currentDepth_ 用于日志和 stepOverDepth_，避免
    // 锁外/锁内两次独立 atomic load 之间 worker 线程更新导致日志与实际不一致。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (stopped_)
            return; // AUDIT-P2 fix: 同 stepIn
        int depth = currentDepth_.load();
        LOG_DEBUG("Step Over (depth=" + std::to_string(depth) + ")", "Debugger");
        mode_.store(static_cast<int>(StepMode::MODE_STEP_OVER));
        stepOverDepth_ = depth;
        crossedDeeper_.store(false); // P0-9 fix: atomic store (DBG-B fix)
        running_ = true;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::stepOut() {
    // P2-5 fix: 同 stepOver，在锁内统一读取 currentDepth_。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (stopped_)
            return; // AUDIT-P2 fix: 同 stepIn
        int depth = currentDepth_.load();
        LOG_DEBUG("Step Out (depth=" + std::to_string(depth) + ")", "Debugger");
        mode_.store(static_cast<int>((depth > 0) ? StepMode::MODE_STEP_OUT : StepMode::MODE_RUN));
        stepOutDepth_ = depth;
        running_ = true;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::resume() {
    LOG_DEBUG("Resume", "Debugger");
    // P1-8 fix: 同 stepIn，统一加锁路径消除 TOCTOU。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (stopped_)
            return; // AUDIT-P2 fix: 同 stepIn
        mode_.store(static_cast<int>(StepMode::MODE_RUN));
        running_ = true;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::stop() {
    LOG_DEBUG("Stop", "Debugger");
    stopped_ = true;
    // AUDIT-P1 fix: 不再在此重置 running_。原 DBG-02 fix 设 running_=false 以便下次启动，
    // 但 checkBreak() 行 33 `if (!running_) return;` 在 stopped_=false 而 running_=false 时
    // 会静默 return 而非 throw DebugStopException，导致 worker 不抛中止异常继续执行下一条语句，
    // UI 状态机不一致。running_ 的重置已由 reset() 行 472 和 prepareRun 系列（行 350/365/380/393）
    // 负责，无需在 stop() 中重复设置。stop() 后 checkBreak 行 29-30 优先检查 stopped_ 抛异常。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        paused_ = false;
        // R98 runToCursor: stop() 清除临时断点。用户主动停止调试时，残留的临时断点
        // 不应跨调试会话存活——下次启动调试时若 tempBreakpointLine_ 仍为旧值，
        // checkBreak 慢速路径会在目标行意外暂停。
        tempBreakpointLine_ = -1;
        hasTempBreakpoint_.store(false);
    }
    pauseCV_.notify_one();
}

// R98 runToCursor: 一次性临时断点 API 实现。
// 临时断点与用户断点独立存储（tempBreakpointLine_ 单值 vs breakpoints_ 集合），
// 命中后立即清除（checkBreak 中处理），不污染 BreakpointConditionPanel 显示。
void DebugController::setTemporaryBreakpoint(int line) {
    if (line <= 0)
        return;
    std::lock_guard<std::mutex> lock(pauseMutex_);
    tempBreakpointLine_ = line;
    hasTempBreakpoint_.store(true);
}

void DebugController::clearTemporaryBreakpoint() {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    tempBreakpointLine_ = -1;
    hasTempBreakpoint_.store(false);
}

int DebugController::getTemporaryBreakpoint() const {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    return tempBreakpointLine_;
}

void DebugController::setCurrentDepth(int depth) {
    currentDepth_.store(depth); // P0-9 fix: atomic store
}

void DebugController::setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb) {
    // P0-9 fix: 加锁保护（getVariableSnapshot 可能在 UI 线程读取）
    std::lock_guard<std::mutex> lock(pauseMutex_);
    variableCallback_ = std::move(cb);
}

void DebugController::setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb) {
    // P0-9 fix: 加锁保护
    std::lock_guard<std::mutex> lock(pauseMutex_);
    callStackCallback_ = std::move(cb);
}

std::vector<VariableSnapshot> DebugController::getVariableSnapshot() const {
    // D-P1-2 fix: 锁内仅拷贝回调函数对象，锁外调用，避免持锁阻塞和重入死锁
    std::function<std::vector<VariableSnapshot>()> cb;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        cb = variableCallback_;
    }
    if (cb) {
        // AUDIT-P1 fix: cb() 调用期间增减活跃计数，供 DebugCoordinator 析构时 spin-wait
        // P0-2 fix: CountGuard 持有 shared_ptr 副本，超时析构后仍安全（atomic 独立存活）。
        activeCallbackCount_->fetch_add(1, std::memory_order_acq_rel);
        struct CountGuard {
            std::shared_ptr<std::atomic<int>> cnt;
            ~CountGuard() { cnt->fetch_sub(1, std::memory_order_acq_rel); }
        } guard{activeCallbackCount_};
        return cb();
    }
    return {};
}

std::vector<CallStackEntry> DebugController::getCallStack() const {
    // D-P1-2 fix: 锁内仅拷贝回调函数对象，锁外调用，避免持锁阻塞和重入死锁
    std::function<std::vector<CallStackEntry>()> cb;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        cb = callStackCallback_;
    }
    if (cb) {
        // AUDIT-P1 fix: cb() 调用期间增减活跃计数，供 DebugCoordinator 析构时 spin-wait
        // P0-2 fix: CountGuard 持有 shared_ptr 副本，超时析构后仍安全（atomic 独立存活）。
        activeCallbackCount_->fetch_add(1, std::memory_order_acq_rel);
        struct CountGuard {
            std::shared_ptr<std::atomic<int>> cnt;
            ~CountGuard() { cnt->fetch_sub(1, std::memory_order_acq_rel); }
        } guard{activeCallbackCount_};
        return cb();
    }
    return {};
}

void DebugController::waitCallbacksIdle() const {
    // AUDIT-P1 fix: spin-wait 直到所有正在执行的 variableCallback_/callStackCallback_ 完成。
    // 用于 DebugCoordinator 析构前安全等待，避免清空 callback 后 worker 线程仍在锁外调用 cb() → UAF。
    // AUDIT-P2-CORRECT fix: 添加超时上限（3 秒）。原实现无限 spin-wait，若 callback 进入
    // 死循环（如条件断点求值包含无限循环），activeCallbackCount_ 永不归零，析构永久阻塞，
    // 进程挂死。超时后记录警告并继续析构（接受可能的 UAF 风险，但优于永久阻塞）。
    // 与 P1-1（条件断点求值超时）配合：若条件求值有步数上限，callback 不会无限循环。
    // P0-2 fix: 计数器为 shared_ptr<atomic>，超时后继续析构不再有 UAF——worker 的 CountGuard
    // 副本保持 atomic 存活至其 fetch_sub 完成释放。超时仅表示放弃等待，不引入悬垂访问。
    constexpr int MAX_WAIT_MS = 3000;
    auto start = std::chrono::steady_clock::now();
    while (activeCallbackCount_->load(std::memory_order_acquire) > 0) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >
            MAX_WAIT_MS) {
            Logger::Warning("waitCallbacksIdle 超时（callback 可能挂死），"
                            "继续析构（计数器为 shared_ptr，worker 完成后安全释放，无 UAF）",
                            "Debugger");
            break;
        }
        std::this_thread::yield();
    }
    // 同步等待条件求值回调完成
    if (evaluator_)
        evaluator_->waitCallbackIdle();
}

bool DebugController::isRunning() const {
    return running_;
}

bool DebugController::isPaused() const {
    // D-P2-14 fix: 结合 stopped_ 判断，避免停止过程中 isPaused 仍返回 true
    // P3.7 fix: 原实现两次独立原子读 (paused_ && !stopped_) 非原子组合，TOCTOU 窗口
    // 内可能返回不一致状态（如 stop() 刚设 stopped_=true 但还未设 paused_=false 时，
    // isPaused 读到 paused_=true + stopped_=true → 返回 false 正确；但若先读到
    // paused_=true 后 stopped_ 才被设为 true，则短暂返回 true → 一帧视觉抖动）。
    // 改用 try_lock 获取一致快照：
    // - 锁获取成功：pauseExecution 未持有 mutex，可一致读取 paused_ 和 stopped_
    // - 锁获取失败：pauseExecution 正在 wait（worker 持有 mutex），说明调试器已暂停
    //   （paused_ 在调用 pauseExecution 前已设为 true），仅检查 stopped_ 即可。
    // try_lock 不会阻塞 UI 线程——pauseExecution 持锁期间正是调试器暂停期间。
    std::unique_lock<std::mutex> lock(pauseMutex_, std::try_to_lock);
    if (lock.owns_lock()) {
        return paused_ && !stopped_;
    }
    // pauseExecution 持有锁 = 调试器暂停中（除非 stop() 已设 stopped_=true 正在唤醒）
    return !stopped_.load(std::memory_order_relaxed);
}

void DebugController::reset() {
    // D-P2-10 fix: 所有状态重置统一在锁内进行，避免锁内外重置的一致性间隙
    //
    // terminate 防御：WorkerManager::forceStop 在 worker 死循环时调用 terminate()，
    // 若 worker 恰在 pauseExecution() 的 wait() 唤醒后重新获取 mutex 的极小窗口内
    // 被杀死，pauseMutex_ 会被死线程持有，lock() 会永久阻塞导致主线程死锁。
    // 改用 try_lock：成功则完整重置；失败则仅重置 atomic 字段（stopped_/running_/
    // paused_ 等关键标志），跳过非原子字段（stepOverDepth_ 等），这些字段会在
    // 下次 stepOver/stepOut 调用时被重新设置，残留值不影响正确性。
    bool locked = false;
    {
        std::unique_lock<std::mutex> lock(pauseMutex_, std::try_to_lock);
        locked = lock.owns_lock();
        mode_.store(static_cast<int>(StepMode::MODE_RUN));
        running_ = false;
        stopped_ = false;
        paused_ = false;
        currentDepth_.store(0);
        lastPausedLine_.store(-1);
        lastPausedDepth_.store(-1);
        lastSeenLine_.store(-1);
        crossedLine_.store(false);
        crossedDeeper_.store(false);
        if (locked) {
            stepOverDepth_ = 0;
            stepOutDepth_ = 0;
            // P0-9 fix: 重置所有断点命中计数在锁内进行（保留断点和条件）
            for (auto it = breakpointInfos_.begin(); it != breakpointInfos_.end(); ++it) {
                it->hitCount = 0;
            }
            // R98 runToCursor: reset() 清除临时断点。新调试会话不应继承上一次会话的
            // runToCursor 目标行，否则首次 RUN 模式 checkBreak 慢速路径会立即触发暂停。
            tempBreakpointLine_ = -1;
            hasTempBreakpoint_.store(false);
        }
    }
    if (!locked) {
        Logger::Warning("DebugController::reset() pauseMutex_ 获取失败（可能被 terminate 的 worker 持有），"
                        "已跳过非原子字段重置",
                        "Debugger");
    }
    pauseCV_.notify_all();
}

void DebugController::pauseExecution() {
    // A2: 使用 condition_variable 替代 QEventLoop
    // worker 线程在此阻塞，UI 线程通过 stepIn/stepOver/resume/stop 唤醒
    // D-P1-3 fix: paused_ 已由调用方在 emit 前设置，此处仅阻塞等待
    std::unique_lock<std::mutex> lock(pauseMutex_);
    pauseCV_.wait(lock, [this] { return !paused_; });
    // 唤醒后 paused_ 已被设为 false
}

void DebugController::updateMinBreakpointLine() {
    minBreakpointLine_ = -1;
    for (int line : breakpoints_) {
        if (minBreakpointLine_ < 0 || line < minBreakpointLine_) {
            minBreakpointLine_ = line;
        }
    }
}
