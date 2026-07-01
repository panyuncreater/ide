// ============================================================
// DebugController — headless (non-Qt) stub for test_harness
// ------------------------------------------------------------
// 实现步进逻辑、记录暂停事件、自动恢复（不阻塞线程）。
// 类名保持 DebugController 以兼容 Interpreter::setDebugger()。
//
// 修复 LNK2005：实现从头文件移到此 .cpp 文件。
// 作为 .obj 直接链接到 debug_test.exe，优先级高于
// minilang_core.lib 中的同名符号，避免多重定义。
// ============================================================

#include "DebugController.h"
#include <algorithm>

// ── Step mode control ──────────────────────────────────────────────

void DebugController::stepIn() {
    mode_ = StepMode::MODE_STEP_IN;
    running_ = true;
    stopped_ = false;
}

void DebugController::stepOver() {
    mode_ = StepMode::MODE_STEP_OVER;
    stepOverDepth_ = currentDepth_;
    running_ = true;
    stopped_ = false;
}

void DebugController::stepOut() {
    mode_ = StepMode::MODE_STEP_OUT;
    stepOutDepth_ = currentDepth_;
    running_ = true;
    stopped_ = false;
}

void DebugController::resume() {
    mode_ = StepMode::MODE_RUN;
    running_ = true;
    stopped_ = false;
}

void DebugController::stop() {
    stopped_ = true;
}

void DebugController::setCurrentDepth(int depth) {
    currentDepth_ = depth;
}

// ── checkBreak ─────────────────────────────────────────────────────

void DebugController::checkBreak(ASTNode* node) {
    if (stopped_) return;

    int line = node->line;

    // Fast path: MODE_RUN with no breakpoints → skip
    if (mode_ == StepMode::MODE_RUN && breakpoints_.empty()) {
        return;
    }

    bool shouldPause = false;

    switch (mode_) {
    case StepMode::MODE_STEP_IN:
        shouldPause = ((line != lastPausedLine_ || currentDepth_ != lastPausedDepth_) && line > 0);
        break;
    case StepMode::MODE_STEP_OVER:
        shouldPause = (currentDepth_ <= stepOverDepth_ && line != lastPausedLine_ && line > 0);
        break;
    case StepMode::MODE_STEP_OUT:
        shouldPause = (currentDepth_ < stepOutDepth_ && line != lastPausedLine_ && line > 0);
        break;
    case StepMode::MODE_RUN:
        shouldPause = breakpoints_.count(line) > 0;
        break;
    }

    if (shouldPause) {
        lastPausedLine_ = line;
        lastPausedDepth_ = currentDepth_;
        pauseCount_++;
        pauseLines_.push_back(line);
        pauseDepths_.push_back(currentDepth_);

        // Record variable snapshot via callback
        DebugPauseEvent evt;
        evt.line = line;
        evt.depth = currentDepth_;
        if (variableCallback_) {
            auto vars = variableCallback_();
            for (auto& v : vars) {
                evt.variables.push_back({v.name, v.value.toString()});
            }
        }
        if (callStackCallback_) {
            auto stack = callStackCallback_();
            for (auto& s : stack) {
                evt.callStack.push_back(s.functionName);
            }
        }
        pauseEvents_.push_back(std::move(evt));
        // Auto-resume (no actual blocking)
    }
}

// ── Breakpoint management ──────────────────────────────────────────

void DebugController::setBreakpoint(int line) { breakpoints_.insert(line); }
void DebugController::removeBreakpoint(int line) { breakpoints_.erase(line); }
bool DebugController::hasBreakpoint(int line) const { return breakpoints_.count(line) > 0; }

void DebugController::setBreakpoints(const std::set<int>& lines) { breakpoints_ = lines; }

void DebugController::setBreakpointCondition(int line, const std::string& cond) {
    breakpointConditions_[line] = cond;
}

void DebugController::setConditionEvaluator(std::function<bool(const std::string&)> eval) {
    conditionEvaluator_ = std::move(eval);
}

// ── Callbacks ──────────────────────────────────────────────────────

void DebugController::setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb) {
    variableCallback_ = std::move(cb);
}

void DebugController::setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb) {
    callStackCallback_ = std::move(cb);
}

std::vector<VariableSnapshot> DebugController::getVariableSnapshot() const {
    return variableCallback_ ? variableCallback_() : std::vector<VariableSnapshot>{};
}

std::vector<CallStackEntry> DebugController::getCallStack() const {
    return callStackCallback_ ? callStackCallback_() : std::vector<CallStackEntry>{};
}

// ── State queries ──────────────────────────────────────────────────

bool DebugController::isRunning() const { return running_; }
bool DebugController::isPaused() const { return false; }  // never actually pauses

void DebugController::reset() {
    mode_ = StepMode::MODE_RUN;
    currentDepth_ = 0;
    stepOverDepth_ = 0;
    stepOutDepth_ = 0;
    lastPausedLine_ = -1;
    lastPausedDepth_ = -1;
    running_ = false;
    stopped_ = false;
    pauseCount_ = 0;
    pauseLines_.clear();
    pauseDepths_.clear();
    pauseEvents_.clear();
}

// ── Test access ────────────────────────────────────────────────────

int DebugController::pauseCount() const { return pauseCount_; }
const std::vector<int>& DebugController::pauseLines() const { return pauseLines_; }
const std::vector<int>& DebugController::pauseDepths() const { return pauseDepths_; }
const std::vector<DebugPauseEvent>& DebugController::pauseEvents() const { return pauseEvents_; }

int DebugController::maxDepthSeen() const {
    int mx = 0;
    for (int d : pauseDepths_) mx = std::max(mx, d);
    return mx;
}
