#include "debug/DebugController.h"
#include "ast/ASTNode.h"
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
}

void DebugController::checkBreak(ASTNode* node) {
    if (!node) return;

    // 如果已被停止，立即终止执行（优先级最高）
    if (stopped_) {
        throw std::runtime_error("调试终止");
    }

    if (!running_) return;

    // 快速路径：RUN 模式且无断点 → 直接返回（递归/循环程序的主要开销来源）
    if (mode_ == StepMode::MODE_RUN && breakpoints_.empty()) {
        return;
    }

    bool shouldPause = false;

    switch (mode_) {
    case StepMode::MODE_RUN:
        // 仅检查断点（先用最小行号快速排除）
        if (node->line >= minBreakpointLine_ && breakpoints_.contains(node->line)) {
            shouldPause = true;
        }
        break;

    case StepMode::MODE_STEP_IN:
        // 只在行号变化时暂停（跳过同行内的子表达式节点）
        if (node->line != lastPausedLine_) {
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
        // 只暂停比进入时更浅的调用深度
        if (currentDepth_ < stepOutDepth_ && node->line != lastPausedLine_) {
            shouldPause = true;
        }
        break;

    case StepMode::MODE_PAUSE:
        // 总是暂停
        shouldPause = true;
        break;
    }

    if (shouldPause && node->line > 0) {
        lastPausedLine_ = node->line;  // 记录暂停行号

        // 发出暂停信号（更新 UI 高亮行）
        emit pausedAt(node->line);

        // 暂停执行，等待用户操作
        pauseExecution();
    }
}

void DebugController::setBreakpoint(int line) {
    breakpoints_.insert(line);
    updateMinBreakpointLine();
}

void DebugController::setBreakpoints(const QSet<int>& lines) {
    breakpoints_ = lines;
    updateMinBreakpointLine();
}

void DebugController::removeBreakpoint(int line) {
    breakpoints_.remove(line);
    updateMinBreakpointLine();
}

void DebugController::toggleBreakpoint(int line) {
    if (breakpoints_.contains(line)) {
        breakpoints_.remove(line);
    } else {
        breakpoints_.insert(line);
    }
    updateMinBreakpointLine();
}

bool DebugController::hasBreakpoint(int line) const {
    return breakpoints_.contains(line);
}

QSet<int> DebugController::getBreakpoints() const {
    return breakpoints_;
}

void DebugController::stepIn() {
    mode_ = StepMode::MODE_STEP_IN;
    running_ = true;
    paused_ = false;       // 解除暂停
    // 不重置 lastPausedLine_：保留当前暂停行号，跳过同行剩余子表达式，
    // 仅在行号变化时才暂停（解决"需按多次才到下一行"的问题）
    // 退出暂停事件循环
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::stepOver() {
    mode_ = StepMode::MODE_STEP_OVER;
    stepOverDepth_ = currentDepth_;
    running_ = true;
    paused_ = false;
    // 同理，不重置 lastPausedLine_
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::stepOut() {
    mode_ = StepMode::MODE_STEP_OUT;
    stepOutDepth_ = currentDepth_;
    running_ = true;
    paused_ = false;
    if (pauseLoop_) pauseLoop_->quit();
}

void DebugController::resume() {
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
    currentDepth_ = 0;
    stepOverDepth_ = 0;
    stepOutDepth_ = 0;
    lastPausedLine_ = -1;
}

void DebugController::pauseExecution() {
    // 设置暂停标志
    paused_ = true;

    // 使用 QEventLoop 替代忙等
    // 事件循环保持 UI 响应，stepIn/stepOver/resume/stop 通过 quit() 唤醒
    QEventLoop loop;
    pauseLoop_ = &loop;
    loop.exec();
    pauseLoop_ = nullptr;
}

void DebugController::updateMinBreakpointLine() {
    minBreakpointLine_ = -1;
    for (int line : breakpoints_) {
        if (minBreakpointLine_ < 0 || line < minBreakpointLine_) {
            minBreakpointLine_ = line;
        }
    }
}
