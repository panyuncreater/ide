#include "debug/DebugController.h"
#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"
#include <QApplication>
#include <QThread>
#include <stdexcept>

// ============================================================
// DebugController 调试控制器实现
// ============================================================


DebugController::DebugController(QObject* parent)
    : QObject(parent) {}

DebugController::~DebugController() {
    stopped_ = true;
    paused_ = false;
    mode_ = StepMode::MODE_RUN;
    if (pauseLoop_) {
        pauseLoop_->quit();
    }
}

void DebugController::checkBreak(ASTNode* node) {
    if (!node) return;

    // 如果已被停止，立即终止执行（优先级最高）
    if (stopped_) {
        throw DebugStopException();
    }

    if (!running_) return;

    // 快速路径：RUN 模式且无断点 → 直接返回（递归/循环程序的主要开销来源）
    if (mode_ == StepMode::MODE_RUN && breakpoints_.empty()) {
        // B11 fix: 即使在快速路径，也定期处理 UI 事件防止界面冻结
        if (++eventPumpCounter_ >= 100) {
            eventPumpCounter_ = 0;
            if (QThread::currentThread() == QCoreApplication::instance()->thread())
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        }
        return;
    }

    bool shouldPause = false;

    switch (mode_) {
    case StepMode::MODE_RUN:
        // C3 fix: 跳过与上次相同行号的子表达式，防止 resume 后同行子节点重复触发断点。
        // lastSeenLine_ 会在执行到其他行时自动更新，使循环下一迭代能重新命中断点。
        if (node->line != lastSeenLine_ &&
            node->line >= minBreakpointLine_ && breakpoints_.contains(node->line)) {
            // 检查是否为条件断点
            auto infoIt = breakpointInfos_.find(node->line);
            if (infoIt != breakpointInfos_.end() && infoIt->isConditional()) {
                // 条件断点：只求值条件为真时才暂停
                if (conditionEvaluator_) {
                    try {
                        if (conditionEvaluator_(infoIt->condition)) {
                            infoIt->hitCount++;  // DB-3 fix: 仅条件满足时递增
                            shouldPause = true;
                        }
                    } catch (...) {
                        // 条件表达式求值异常——视为条件不满足，不暂停
                    }
                }
            } else {
                // 无条件断点：直接暂停
                if (infoIt != breakpointInfos_.end()) {
                    infoIt->hitCount++;
                }
                shouldPause = true;
            }
        }
        break;

    case StepMode::MODE_STEP_IN:
        // 行号变化时暂停（跳过同行子表达式），或调用深度变化时暂停（递归函数同行不同深度）
        if (node->line != lastPausedLine_ || currentDepth_ != lastPausedDepth_) {
            shouldPause = true;
        }
        break;

    case StepMode::MODE_STEP_OVER:
        // 只暂停同一或更浅调用深度，且行号变化的节点
        if (currentDepth_ <= stepOverDepth_ && node->line != lastPausedLine_) {
            shouldPause = true;
        }
        break;

    case StepMode::MODE_STEP_OUT:
        // H6 fix: 仅检查调用深度，不使用 lastPausedLine_ 防护。
        // 深度变浅即可确定已从函数返回，同行嵌套调用或递归函数也能正确暂停。
        if (currentDepth_ < stepOutDepth_) {
            shouldPause = true;
        }
        break;
    }

    // C3 fix: 始终记录最后看到的行号，使执行移到其他行后 lastSeenLine_ 自动更新
    if (node->line > 0) {
        lastSeenLine_ = node->line;
    }

    if (shouldPause && node->line > 0) {
        lastPausedLine_ = node->line;  // 记录暂停行号
        lastPausedDepth_ = currentDepth_;  // 记录暂停深度

        // 发出暂停信号（更新 UI 高亮行）
        emit pausedAt(node->line);

        // 暂停执行，等待用户操作
        pauseExecution();
    } else {
        // B11 fix: 不暂停时也定期处理 UI 事件，防止界面冻结
        if (++eventPumpCounter_ >= 100) {
            eventPumpCounter_ = 0;
            if (QThread::currentThread() == QCoreApplication::instance()->thread())
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        }
    }
}

void DebugController::setBreakpoint(int line) {
    breakpoints_.insert(line);
    // 若尚无 BreakpointInfo 条目则创建（保留已有条件）
    if (!breakpointInfos_.contains(line)) {
        breakpointInfos_.insert(line, BreakpointInfo(line));
    }
    updateMinBreakpointLine();
}

void DebugController::setBreakpoints(const QSet<int>& lines) {
    breakpoints_ = lines;
    // 同步 breakpointInfos_：移除不再存在的，添加新增的
    auto it = breakpointInfos_.begin();
    while (it != breakpointInfos_.end()) {
        if (!lines.contains(it.key())) {
            it = breakpointInfos_.erase(it);
        } else {
            ++it;
        }
    }
    for (int line : lines) {
        if (!breakpointInfos_.contains(line)) {
            breakpointInfos_.insert(line, BreakpointInfo(line));
        }
    }
    updateMinBreakpointLine();
}

void DebugController::removeBreakpoint(int line) {
    breakpoints_.remove(line);
    breakpointInfos_.remove(line);
    updateMinBreakpointLine();
}

void DebugController::toggleBreakpoint(int line) {
    if (breakpoints_.contains(line)) {
        breakpoints_.remove(line);
        breakpointInfos_.remove(line);
    } else {
        breakpoints_.insert(line);
        if (!breakpointInfos_.contains(line)) {
            breakpointInfos_.insert(line, BreakpointInfo(line));
        }
    }
    updateMinBreakpointLine();
}

bool DebugController::hasBreakpoint(int line) const {
    return breakpoints_.contains(line);
}

QSet<int> DebugController::getBreakpoints() const {
    return breakpoints_;
}

void DebugController::setBreakpointCondition(int line, const std::string& condition) {
    // 确保断点存在
    if (!breakpoints_.contains(line)) {
        setBreakpoint(line);
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
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        return it->condition;
    }
    return "";
}

int DebugController::getBreakpointHitCount(int line) const {
    auto it = breakpointInfos_.find(line);
    if (it != breakpointInfos_.end()) {
        return it->hitCount;
    }
    return 0;
}

void DebugController::setConditionEvaluator(std::function<bool(const std::string&)> evaluator) {
    conditionEvaluator_ = std::move(evaluator);
}

void DebugController::stepIn() {
    // 初始模式设置：尚未开始执行，只设置模式不退出事件循环
    if (!running_) {
        mode_ = StepMode::MODE_STEP_IN;
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
    // 防重入：只有暂停状态才能步进
    if (!inPauseLoop_) return;
    mode_ = StepMode::MODE_STEP_IN;
    running_ = true;
    stopped_ = false;
    paused_ = false;
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::stepOver() {
    if (!running_) {
        mode_ = StepMode::MODE_STEP_OVER;
        stepOverDepth_ = currentDepth_;
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
    if (!inPauseLoop_) return;
    mode_ = StepMode::MODE_STEP_OVER;
    stepOverDepth_ = currentDepth_;
    running_ = true;
    stopped_ = false;
    paused_ = false;
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::stepOut() {
    if (!running_) {
        // depth 0 时无外层作用域可跳出，降级为 Resume
        mode_ = (currentDepth_ > 0) ? StepMode::MODE_STEP_OUT : StepMode::MODE_RUN;
        stepOutDepth_ = currentDepth_;
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
    if (!inPauseLoop_) return;
    mode_ = (currentDepth_ > 0) ? StepMode::MODE_STEP_OUT : StepMode::MODE_RUN;
    stepOutDepth_ = currentDepth_;
    running_ = true;
    stopped_ = false;
    paused_ = false;
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::resume() {
    if (!running_) {
        mode_ = StepMode::MODE_RUN;
        running_ = true;
        stopped_ = false;
        paused_ = false;
        return;
    }
    if (!inPauseLoop_) return;
    mode_ = StepMode::MODE_RUN;
    running_ = true;
    stopped_ = false;
    paused_ = false;
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::stop() {
    stopped_ = true;
    paused_ = false;
    // 注意：不设 running_ = false，让 checkBreak 能走到 stopped_ 检查
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::setCurrentDepth(int depth) {
    currentDepth_ = depth;
}

void DebugController::setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb) {
    variableCallback_ = cb;
}

void DebugController::setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb) {
    callStackCallback_ = cb;
}

std::vector<VariableSnapshot> DebugController::getVariableSnapshot() const {
    if (variableCallback_) return variableCallback_();
    return {};
}

std::vector<CallStackEntry> DebugController::getCallStack() const {
    if (callStackCallback_) return callStackCallback_();
    return {};
}

bool DebugController::isRunning() const {
    return running_;
}

bool DebugController::isPaused() const {
    return paused_;
}

void DebugController::reset() {
    mode_ = StepMode::MODE_RUN;
    running_ = false;
    stopped_ = false;
    paused_ = false;
    // 先退出暂停事件循环，再清理指针
    if (pauseLoop_) {
        pauseLoop_->quit();
    }
    inPauseLoop_ = false;
    pauseLoop_ = nullptr;
    currentDepth_ = 0;
    stepOverDepth_ = 0;
    stepOutDepth_ = 0;
    lastPausedLine_ = -1;
    lastPausedDepth_ = -1;
    lastSeenLine_ = -1;

    // 重置所有断点命中计数（保留断点和条件）
    for (auto it = breakpointInfos_.begin(); it != breakpointInfos_.end(); ++it) {
        it->hitCount = 0;
    }
}

void DebugController::pauseExecution() {
    // 防止嵌套事件循环重入（use-after-free 风险）
    if (inPauseLoop_) {
        return;
    }
    
    // 设置暂停标志
    paused_ = true;
    inPauseLoop_ = true;

    // 使用 QEventLoop 替代忙等
    // 事件循环保持 UI 响应，stepIn/stepOver/resume/stop 通过 quit() 唤醒
    QEventLoop loop;
    pauseLoop_ = &loop;
    loop.exec();
    pauseLoop_ = nullptr;
    inPauseLoop_ = false;
    paused_ = false;  // 安全防护：确保状态一致
}

void DebugController::updateMinBreakpointLine() {
    minBreakpointLine_ = -1;
    for (int line : breakpoints_) {
        if (minBreakpointLine_ < 0 || line < minBreakpointLine_) {
            minBreakpointLine_ = line;
        }
    }
}
