#include "debug/DebugController.h"
#include "ast/ASTNode.h"
#include "interpreter/RuntimeExceptions.h"  // S6 fix: DebugStopException 定义
#include "Logger.h"
#include <stdexcept>

// ============================================================
// DebugController 调试控制器实现
// ============================================================


DebugController::DebugController(QObject* parent)
    : QObject(parent) {}

DebugController::~DebugController() {
    stopped_ = true;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        paused_ = false;
        mode_.store(static_cast<int>(StepMode::MODE_RUN));
    }
    pauseCV_.notify_all();
}

void DebugController::checkBreak(ASTNode* node) {
    if (!node) return;

    // 如果已被停止，立即终止执行（优先级最高）
    if (stopped_) {
        throw DebugStopException();
    }

    if (!running_) return;

    // D-P1-1 fix: 原子快速路径——RUN 模式且无断点时，无锁返回
    if (static_cast<StepMode>(mode_.load()) == StepMode::MODE_RUN &&
        !hasBreakpoints_.load()) {
        // D-P2-11 fix: 快速路径也更新行号追踪
        updateLineTracking(node->line, 0, 0, StepMode::MODE_RUN);
        return;
    }

    // Thread-safety: take a snapshot of shared state under the mutex.
    StepMode snapMode;
    QSet<int> localBreakpoints;
    QMap<int, BreakpointInfo> localBreakpointInfos;
    int snapCurrentDepth, snapStepOverDepth, snapStepOutDepth;
    int snapLastPausedLine, snapLastPausedDepth, snapMinBreakpointLine;
    bool snapCrossedDeeper;
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
    }

    // 慢速路径中的二次确认（hasBreakpoints_ 原子读可能与锁内状态有微小窗口）
    if (snapMode == StepMode::MODE_RUN && localBreakpoints.empty()) {
        updateLineTracking(node->line, 0, 0, StepMode::MODE_RUN);
        return;
    }

    // DBG-B fix: Step Over 模式下追踪是否进入了更深的调用层
    if (snapMode == StepMode::MODE_STEP_OVER && snapCurrentDepth > snapStepOverDepth) {
        crossedDeeper_.store(true);
        snapCrossedDeeper = true;
    }

    // C11 fix: 委托给助手方法判断是否应暂停
    bool shouldPause = false;
    if (snapMode == StepMode::MODE_RUN) {
        shouldPause = shouldPauseAtBreakpoint(node->line, localBreakpoints,
                                              localBreakpointInfos, snapMinBreakpointLine);
    } else {
        shouldPause = shouldPauseForStepping(snapMode, node->line, snapCurrentDepth,
                                             snapStepOverDepth, snapStepOutDepth,
                                             snapLastPausedLine, snapLastPausedDepth,
                                             snapCrossedDeeper);
    }

    // C3 fix + DBG-03: 始终记录最后看到的行号
    updateLineTracking(node->line, snapCurrentDepth, snapStepOverDepth, snapMode);

    // M10 + DBG-04 fix: 步进模式下经过断点行时递增 hitCount
    if (snapMode != StepMode::MODE_RUN && node->line > 0) {
        if (node->line != snapLastPausedLine || snapCurrentDepth != snapLastPausedDepth) {
            std::lock_guard<std::mutex> lock(pauseMutex_);
            auto realIt = breakpointInfos_.find(node->line);
            if (realIt != breakpointInfos_.end()) {
                realIt->hitCount++;
            }
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
    if ((line != lastSeenLine_.load() || crossedLine_.load()) &&
        line >= snapMinBreakpointLine && localBreakpoints.contains(line)) {
        crossedLine_.store(false);  // 命中后重置，同行后续子表达式不再触发
        // 检查是否为条件断点
        auto infoIt = localBreakpointInfos.find(line);
        if (infoIt != localBreakpointInfos.end() && infoIt->isConditional()) {
            // A5 fix: 委托给 DebugEvaluator 求值（异常处理 + 日志已封装）
            if (evaluator_ && evaluator_->hasCallback()) {
                if (evaluator_->evaluate(infoIt->condition, line)) {
                    {
                        std::lock_guard<std::mutex> lock(pauseMutex_);
                        auto realIt = breakpointInfos_.find(line);
                        if (realIt != breakpointInfos_.end()) realIt->hitCount++;
                    }
                    return true;
                }
            }
        } else {
            // 无条件断点：直接暂停
            if (infoIt != localBreakpointInfos.end()) {
                std::lock_guard<std::mutex> lock(pauseMutex_);
                auto realIt = breakpointInfos_.find(line);
                if (realIt != breakpointInfos_.end()) realIt->hitCount++;
            }
            return true;
        }
    }
    return false;
}

bool DebugController::shouldPauseForStepping(StepMode snapMode, int line, int snapCurrentDepth,
                                             int snapStepOverDepth, int snapStepOutDepth,
                                             int snapLastPausedLine, int snapLastPausedDepth,
                                             bool snapCrossedDeeper) {
    (void)line;  // STEP_OUT 不使用 line
    switch (snapMode) {
    case StepMode::MODE_STEP_IN:
        // 行号变化时暂停，或调用深度变化时暂停（递归函数同行不同深度）
        return (line != snapLastPausedLine || snapCurrentDepth != snapLastPausedDepth);
    case StepMode::MODE_STEP_OVER:
        // 只暂停同一或更浅调用深度，且行号变化或从深层返回
        if (snapCurrentDepth <= snapStepOverDepth &&
            (line != snapLastPausedLine || snapCrossedDeeper)) {
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

void DebugController::updateLineTracking(int line, int snapCurrentDepth, int snapStepOverDepth,
                                         StepMode snapMode) {
    (void)snapCurrentDepth;
    (void)snapStepOverDepth;
    (void)snapMode;
    if (line > 0) {
        if (line != lastSeenLine_.load()) crossedLine_.store(true);
        lastSeenLine_.store(line);
    }
}

void DebugController::doPause(int line, int snapCurrentDepth) {
    lastPausedLine_.store(line);
    lastPausedDepth_.store(snapCurrentDepth);
    crossedDeeper_.store(false);  // DBG-B fix: 暂停后重置

    // Perf-LazyLog: LOG_DEBUG 宏级别过滤后跳过字符串构造（断点命中频繁时收益明显）
    LOG_DEBUG("断点暂停于行 " + std::to_string(line) +
        " (深度 " + std::to_string(snapCurrentDepth) + ")", "Debugger");

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
    if (stopped_) throw DebugStopException();
}

void DebugController::setBreakpoint(int line) {
    // D-P2-6 fix: 校验行号有效性（负数或 0 无意义）
    if (line <= 0) return;
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
        if (line > 0) validLines.insert(line);
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
    if (line <= 0) return;
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
    if (line <= 0) return;
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
        it->hitCount = 0;  // 条件变更时重置命中计数
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

void DebugController::stepIn() {
    LOG_DEBUG("Step In", "Debugger");
    // P1-8 fix: 消除 TOCTOU。原实现先无锁检查 !running_ 决定走快速路径（无 CV notify），
    // 再加锁走慢速路径。检查与加锁之间存在窗口，且快速路径不 notify 在 worker
    // 极端时序下可能丢唤醒。改为统一加锁路径，notify 对无 waiter 是 no-op。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        mode_.store(static_cast<int>(StepMode::MODE_STEP_IN));
        running_ = true;
        stopped_ = false;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::stepOver() {
    LOG_DEBUG("Step Over (depth=" + std::to_string(currentDepth_.load()) + ")", "Debugger");
    // P1-8 fix: 同 stepIn，统一加锁路径消除 TOCTOU。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        mode_.store(static_cast<int>(StepMode::MODE_STEP_OVER));
        stepOverDepth_ = currentDepth_.load();  // P0-9 fix: atomic load
        crossedDeeper_.store(false);            // P0-9 fix: atomic store (DBG-B fix)
        running_ = true;
        stopped_ = false;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::stepOut() {
    LOG_DEBUG("Step Out (depth=" + std::to_string(currentDepth_.load()) + ")", "Debugger");
    int depth = currentDepth_.load();  // P0-9 fix: atomic load
    // P1-8 fix: 同 stepIn，统一加锁路径消除 TOCTOU。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        mode_.store(static_cast<int>((depth > 0) ? StepMode::MODE_STEP_OUT : StepMode::MODE_RUN));
        stepOutDepth_ = depth;
        running_ = true;
        stopped_ = false;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::resume() {
    LOG_DEBUG("Resume", "Debugger");
    // P1-8 fix: 同 stepIn，统一加锁路径消除 TOCTOU。
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        mode_.store(static_cast<int>(StepMode::MODE_RUN));
        running_ = true;
        stopped_ = false;
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::stop() {
    LOG_DEBUG("Stop", "Debugger");
    stopped_ = true;
    running_ = false;  // DBG-02 fix: 重置 running_ 以便下次启动时能正确初始化步进模式
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        paused_ = false;
    }
    pauseCV_.notify_one();
}

void DebugController::setCurrentDepth(int depth) {
    currentDepth_.store(depth);  // P0-9 fix: atomic store
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
    if (cb) return cb();
    return {};
}

std::vector<CallStackEntry> DebugController::getCallStack() const {
    // D-P1-2 fix: 锁内仅拷贝回调函数对象，锁外调用，避免持锁阻塞和重入死锁
    std::function<std::vector<CallStackEntry>()> cb;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        cb = callStackCallback_;
    }
    if (cb) return cb();
    return {};
}

bool DebugController::isRunning() const {
    return running_;
}

bool DebugController::isPaused() const {
    // D-P2-14 fix: 结合 stopped_ 判断，避免停止过程中 isPaused 仍返回 true
    return paused_ && !stopped_;
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
        }
    }
    if (!locked) {
        Logger::Warning("DebugController::reset() pauseMutex_ 获取失败（可能被 terminate 的 worker 持有），"
                        "已跳过非原子字段重置", "Debugger");
    }
    pauseCV_.notify_all();
}

void DebugController::pauseExecution() {
    // A2: 使用 condition_variable 替代 QEventLoop
    // worker 线程在此阻塞，UI 线程通过 stepIn/stepOver/resume/stop 唤醒
    // D-P1-3 fix: paused_ 已由调用方在 emit 前设置，此处仅阻塞等待
    std::unique_lock<std::mutex> lock(pauseMutex_);
    pauseCV_.wait(lock, [this]{ return !paused_; });
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
