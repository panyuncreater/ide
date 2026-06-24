#include "debug/DebugController.h"
#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"
#include "Logger.h"
#include <QCoreApplication>  // P1 fix: 用 QCoreApplication 替代 QApplication，避免 minilang_core 依赖 Qt6::Widgets
#include <QThread>
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
    // 避免每次 checkBreak 都获取 mutex 并深拷贝断点容器（循环/递归程序的主要开销）
    if (static_cast<StepMode>(mode_.load()) == StepMode::MODE_RUN &&
        !hasBreakpoints_.load()) {
        // D-P2-11 fix: 快速路径也更新 lastSeenLine_/crossedLine_，
        // 避免动态添加断点后切换到慢速路径时状态陈旧导致断点漏触发
        if (node->line > 0) {
            if (node->line != lastSeenLine_.load()) crossedLine_.store(true);
            lastSeenLine_.store(node->line);
        }
        // B11 fix: 即使在快速路径，也定期处理 UI 事件防止界面冻结
        if (++eventPumpCounter_ >= 100) {
            eventPumpCounter_ = 0;
            // D-P2-3 fix: QCoreApplication::instance() 可能为 null（单元测试/库使用场景）
            auto app = QCoreApplication::instance();
            if (app && QThread::currentThread() == app->thread())
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        }
        return;
    }

    // Thread-safety: take a snapshot of shared state under the mutex.
    // Breakpoint containers are copied; scalar fields are read into locals.
    // This avoids holding the mutex for the entire function.
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
        snapCurrentDepth = currentDepth_.load();       // P0-9 fix: atomic load
        snapStepOverDepth = stepOverDepth_;
        snapStepOutDepth = stepOutDepth_;
        snapLastPausedLine = lastPausedLine_.load();   // P0-9 fix: atomic load
        snapLastPausedDepth = lastPausedDepth_.load(); // P0-9 fix: atomic load
        snapMinBreakpointLine = minBreakpointLine_;
        snapCrossedDeeper = crossedDeeper_.load();     // P0-9 fix: atomic load
    }

    // 快速路径：RUN 模式且无断点 → 直接返回（递归/循环程序的主要开销来源）
    // 注意：此处保留原逻辑作为慢速路径中的二次确认（hasBreakpoints_ 是原子读，可能与锁内状态有微小窗口）
    if (snapMode == StepMode::MODE_RUN && localBreakpoints.empty()) {
        // D-P2-11 fix: 快速路径也更新行号追踪
        if (node->line > 0) {
            if (node->line != lastSeenLine_.load()) crossedLine_.store(true);
            lastSeenLine_.store(node->line);
        }
        // B11 fix: 即使在快速路径，也定期处理 UI 事件防止界面冻结
        if (++eventPumpCounter_ >= 100) {
            eventPumpCounter_ = 0;
            auto app = QCoreApplication::instance();
            if (app && QThread::currentThread() == app->thread())
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        }
        return;
    }

    bool shouldPause = false;

    // DBG-B fix: Step Over 模式下追踪是否进入了更深的调用层
    if (snapMode == StepMode::MODE_STEP_OVER && snapCurrentDepth > snapStepOverDepth) {
        crossedDeeper_.store(true);  // P0-9 fix: atomic store
        snapCrossedDeeper = true;
    }

    switch (snapMode) {
    case StepMode::MODE_RUN:
        // C3 fix: 跳过与上次相同行号的子表达式，防止 resume 后同行子节点重复触发断点。
        // lastSeenLine_ 会在执行到其他行时自动更新，使循环下一迭代能重新命中断点。
        // DBG-03 fix: 单行循环断点重触发——用 crossedLine_ 检测是否跨过不同行
        if ((node->line != lastSeenLine_.load() || crossedLine_.load()) &&
            node->line >= snapMinBreakpointLine && localBreakpoints.contains(node->line)) {
            crossedLine_.store(false);  // P0-9 fix: atomic store. 命中后重置，同行后续子表达式不再触发
            // 检查是否为条件断点
            auto infoIt = localBreakpointInfos.find(node->line);
            if (infoIt != localBreakpointInfos.end() && infoIt->isConditional()) {
                // 条件断点：只求值条件为真时才暂停
                // P0-9 fix: 在 mutex 下读取 conditionEvaluator_ 快照，避免与 setConditionEvaluator 竞争
                std::function<bool(const std::string&)> snapEvaluator;
                {
                    std::lock_guard<std::mutex> lock(pauseMutex_);
                    snapEvaluator = conditionEvaluator_;
                }
                if (snapEvaluator) {
                    try {
                        if (snapEvaluator(infoIt->condition)) {
                            // Thread-safety: update hitCount on the real container under mutex
                            {
                                std::lock_guard<std::mutex> lock(pauseMutex_);
                                auto realIt = breakpointInfos_.find(node->line);
                                if (realIt != breakpointInfos_.end()) realIt->hitCount++;
                            }
                            shouldPause = true;
                        }
                    } catch (...) {
                        // 条件表达式求值异常——视为条件不满足，不暂停
                    }
                }
            } else {
                // 无条件断点：直接暂停
                if (infoIt != localBreakpointInfos.end()) {
                    std::lock_guard<std::mutex> lock(pauseMutex_);
                    auto realIt = breakpointInfos_.find(node->line);
                    if (realIt != breakpointInfos_.end()) realIt->hitCount++;
                }
                shouldPause = true;
            }
        }
        break;

    case StepMode::MODE_STEP_IN:
        // 行号变化时暂停（跳过同行子表达式），或调用深度变化时暂停（递归函数同行不同深度）
        if (node->line != snapLastPausedLine || snapCurrentDepth != snapLastPausedDepth) {
            shouldPause = true;
        }
        break;

    case StepMode::MODE_STEP_OVER:
        // 只暂停同一或更浅调用深度，且行号变化或从深层返回的节点
        // DBG-B fix: crossedDeeper_ 检测从函数调用返回 — 即使同行也暂停（f();g(); 场景）
        if (snapCurrentDepth <= snapStepOverDepth && (node->line != snapLastPausedLine || snapCrossedDeeper)) {
            shouldPause = true;
        }
        break;

    case StepMode::MODE_STEP_OUT:
        // H6 fix: 仅检查调用深度，不使用 lastPausedLine_ 防护。
        // 深度变浅即可确定已从函数返回，同行嵌套调用或递归函数也能正确暂停。
        if (snapCurrentDepth < snapStepOutDepth) {
            shouldPause = true;
        }
        break;
    }

    // C3 fix + DBG-03: 始终记录最后看到的行号，跨行时设置 crossedLine_ 允许单行循环断点重触发
    if (node->line > 0) {
        if (node->line != lastSeenLine_.load()) crossedLine_.store(true);  // P0-9 fix: atomic
        lastSeenLine_.store(node->line);                                    // P0-9 fix: atomic
    }

    // M10 + DBG-04 fix: 步进模式下经过断点行时递增 hitCount，去重避免同行多个子表达式重复计数
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
        lastPausedLine_.store(node->line);           // P0-9 fix: atomic store
        lastPausedDepth_.store(snapCurrentDepth);    // P0-9 fix: atomic store
        crossedDeeper_.store(false);                 // P0-9 fix: atomic store (DBG-B fix: 暂停后重置)

        Logger::Debug("断点暂停于行 " + std::to_string(node->line) +
            " (深度 " + std::to_string(snapCurrentDepth) + ")", "Debugger");

        // D-P1-3 fix: 先设置 paused_=true（锁内），再发信号，最后阻塞等待。
        // 确保 UI 收到信号查询变量时，worker 已确认进入暂停状态，避免解释器状态竞争窗口。
        {
            std::lock_guard<std::mutex> lock(pauseMutex_);
            paused_ = true;
        }
        // 发出暂停信号（更新 UI 高亮行）——此时 paused_ 已为 true
        emit pausedAt(node->line);
        // 阻塞等待用户操作（pauseExecution 内部不再重复设置 paused_）
        pauseExecution();

        // D-P2-1 fix: pauseExecution 返回后立即检查 stopped_，
        // 避免用户停止后仍执行当前节点的副作用（如 print 输出）
        if (stopped_) throw DebugStopException();
    } else {
        // B11 fix: 不暂停时也定期处理 UI 事件，防止界面冻结
        if (++eventPumpCounter_ >= 100) {
            eventPumpCounter_ = 0;
            // D-P2-3 fix: QCoreApplication::instance() 可能为 null
            auto app = QCoreApplication::instance();
            if (app && QThread::currentThread() == app->thread())
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        }
    }
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
    // P0-9 fix: 加锁保护（checkBreak 在 worker 线程读取）
    std::lock_guard<std::mutex> lock(pauseMutex_);
    conditionEvaluator_ = std::move(evaluator);
}

void DebugController::stepIn() {
    Logger::Debug("Step In", "Debugger");
    // 初始模式设置：尚未开始执行
    if (!running_) {
        mode_.store(static_cast<int>(StepMode::MODE_STEP_IN));
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
    // A2: 通过 CV 唤醒 worker 线程
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
    Logger::Debug("Step Over (depth=" + std::to_string(currentDepth_.load()) + ")", "Debugger");
    if (!running_) {
        mode_.store(static_cast<int>(StepMode::MODE_STEP_OVER));
        stepOverDepth_ = currentDepth_.load();  // P0-9 fix: atomic load
        crossedDeeper_.store(false);            // P0-9 fix: atomic store (DBG-B fix)
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
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
    Logger::Debug("Step Out (depth=" + std::to_string(currentDepth_.load()) + ")", "Debugger");
    int depth = currentDepth_.load();  // P0-9 fix: atomic load
    if (!running_) {
        mode_.store(static_cast<int>((depth > 0) ? StepMode::MODE_STEP_OUT : StepMode::MODE_RUN));
        stepOutDepth_ = depth;
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
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
    Logger::Debug("Resume", "Debugger");
    if (!running_) {
        mode_.store(static_cast<int>(StepMode::MODE_RUN));
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
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
    Logger::Debug("Stop", "Debugger");
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
    // D-P2-8 fix: eventPumpCounter_ 也需重置，避免跨调试会话残留
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        mode_.store(static_cast<int>(StepMode::MODE_RUN));
        running_ = false;
        stopped_ = false;
        paused_ = false;
        stepOverDepth_ = 0;
        stepOutDepth_ = 0;
        currentDepth_.store(0);
        lastPausedLine_.store(-1);
        lastPausedDepth_.store(-1);
        lastSeenLine_.store(-1);
        crossedLine_.store(false);
        crossedDeeper_.store(false);  // DBG-B fix
        eventPumpCounter_ = 0;        // D-P2-8 fix: 重置事件泵计数器
        // P0-9 fix: 重置所有断点命中计数在锁内进行（保留断点和条件）
        for (auto it = breakpointInfos_.begin(); it != breakpointInfos_.end(); ++it) {
            it->hitCount = 0;
        }
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
