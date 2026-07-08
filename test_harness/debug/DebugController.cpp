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

// 进入「单步进入」模式：每执行一条语句即暂停，并重置交叉帧标志开启新一轮步进。
void DebugController::stepIn() {
    mode_ = StepMode::MODE_STEP_IN;
    running_ = true;
    stopped_ = false;
    crossedDeeper_ = false;  // BUG-DBG-14 fix: 新步进开始，重置交叉帧标志
}

// 进入「单步跳过」模式：记录当前栈深度，函数调用不进入；返回后即使行号不变也因 crossedDeeper_ 而暂停。
void DebugController::stepOver() {
    mode_ = StepMode::MODE_STEP_OVER;
    stepOverDepth_ = currentDepth_;
    running_ = true;
    stopped_ = false;
    crossedDeeper_ = false;  // BUG-DBG-14 fix: 新步进开始，重置交叉帧标志
}

// 进入「单步跳出」模式：运行至从当前函数帧返回到更浅帧时暂停。
void DebugController::stepOut() {
    mode_ = StepMode::MODE_STEP_OUT;
    stepOutDepth_ = currentDepth_;
    running_ = true;
    stopped_ = false;
    crossedDeeper_ = false;  // BUG-DBG-14 fix: 新步进开始，重置交叉帧标志
}

// 恢复连续运行（MODE_RUN）：不再逐步暂停，直到命中断点或收到 stop。
void DebugController::resume() {
    mode_ = StepMode::MODE_RUN;
    running_ = true;
    stopped_ = false;
    crossedDeeper_ = false;  // BUG-DBG-14 fix: 新步进开始，重置交叉帧标志
}

// 请求停止：置 stopped_ 标志，checkBreak 将立即返回、不再暂停。
void DebugController::stop() {
    stopped_ = true;
}

// 由解释器回调设置当前调用栈深度，供 STEP_OVER / STEP_OUT 判定帧边界。
void DebugController::setCurrentDepth(int depth) {
    currentDepth_ = depth;
}

// ── checkBreak ─────────────────────────────────────────────────────

// 核心暂停判定：根据当前步进模式（STEP_IN/OVER/OUT/RUN）结合断点、行号去重与交叉帧标志，
// 决定是否在当前 AST 节点暂停；命中时记录行号、深度与变量/调用栈快照，随后自动恢复（不阻塞）。
void DebugController::checkBreak(ASTNode* node) {
    if (stopped_) return;

    int line = node->line;

    // Fast path: MODE_RUN with no breakpoints → skip
    if (mode_ == StepMode::MODE_RUN && breakpoints_.empty()) {
        return;
    }

    // BUG-DBG-14 fix: crossedLine_ 机制（F4 fix）—— 单行循环断点重新触发。
    // 原实现仅用 line != lastPausedLine_ 去重，单行循环断点首次命中后永不再触发。
    if (line > 0) {
        if (line != lastSeenLine_) {
            crossedLine_ = true;
        }
        lastSeenLine_ = line;
    }

    bool shouldPause = false;

    // 按步进模式分派暂停判定：STEP_IN 逐条暂停；STEP_OVER 在当前深度内暂停且允许同行动跨帧返回后暂停；
    // STEP_OUT 在返回更浅栈帧时暂停；MODE_RUN 仅检查断点（含条件断点）与行号去重。
    switch (mode_) {
    case StepMode::MODE_STEP_IN:
        shouldPause = ((line != lastPausedLine_ || currentDepth_ != lastPausedDepth_) && line > 0);
        break;
    case StepMode::MODE_STEP_OVER:
        // BUG-DBG-14 fix: crossedDeeper_ 机制（DBG-B fix）——
        // 同行函数调用（如 foo(); bar(); 同在第5行）STEP_OVER foo() 后
        // currentLine 仍为 5 == lastPausedLine_，原逻辑不暂停直接执行 bar()。
        // crossedDeeper_ 确保从更深帧返回后即使行号不变也暂停。
        if (currentDepth_ > stepOverDepth_) {
            crossedDeeper_ = true;
        }
        if (currentDepth_ <= stepOverDepth_) {
            shouldPause = (line > 0 && (line != lastPausedLine_ || crossedDeeper_));
        }
        break;
    case StepMode::MODE_STEP_OUT:
        // BUG-DBG-14 fix: 顶层 STEP_OUT 用 crossedLine_ 允许单行循环暂停
        if (currentDepth_ < stepOutDepth_) {
            shouldPause = true;
        } else if (currentDepth_ <= 1 && line > 0
                   && (line != lastPausedLine_ || crossedLine_)) {
            shouldPause = true;
        }
        break;
    case StepMode::MODE_RUN:
        // BUG-DBG-14 fix: 条件断点求值 + crossedLine_ 机制
        if (breakpoints_.count(line) > 0) {
            // 检查是否有条件
            auto condIt = breakpointConditions_.find(line);
            if (condIt == breakpointConditions_.end() || condIt->second.empty()) {
                // 无条件断点：用 crossedLine_ 避免同行重复触发
                shouldPause = (line != lastPausedLine_ || crossedLine_);
            } else if (conditionEvaluator_) {
                // 条件断点：求值为真才暂停，用 crossedLine_ 允许单行循环重新触发
                shouldPause = conditionEvaluator_(condIt->second)
                    && (line != lastPausedLine_ || crossedLine_);
            }
            // 无求值器时视为条件不满足（不暂停）
        }
        break;
    }

    if (shouldPause) {
        lastPausedLine_ = line;
        lastPausedDepth_ = currentDepth_;
        crossedLine_ = false;  // 命中后重置，同行后续指令不再触发
        crossedDeeper_ = false;  // 命中后重置
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

// 断点增删查：以行号为键维护断点集合。
void DebugController::setBreakpoint(int line) { breakpoints_.insert(line); }
void DebugController::removeBreakpoint(int line) { breakpoints_.erase(line); }
bool DebugController::hasBreakpoint(int line) const { return breakpoints_.count(line) > 0; }

// 批量覆盖式设置断点集合。
void DebugController::setBreakpoints(const std::set<int>& lines) { breakpoints_ = lines; }

// 为指定行号的断点附加条件表达式（字符串形式）。
void DebugController::setBreakpointCondition(int line, const std::string& cond) {
    breakpointConditions_[line] = cond;
}

// 设置条件断点的求值器（lambda），在命中断点时求值决定是否满足暂停条件。
void DebugController::setConditionEvaluator(std::function<bool(const std::string&)> eval) {
    conditionEvaluator_ = std::move(eval);
}

// ── Callbacks ──────────────────────────────────────────────────────

// 注册变量快照回调，暂停时通过该回调采集当前作用域变量。
void DebugController::setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb) {
    variableCallback_ = std::move(cb);
}

// 注册调用栈回调，暂停时采集当前函数调用链。
void DebugController::setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb) {
    callStackCallback_ = std::move(cb);
}

// 取变量快照；若回调未注册则返回空。
std::vector<VariableSnapshot> DebugController::getVariableSnapshot() const {
    return variableCallback_ ? variableCallback_() : std::vector<VariableSnapshot>{};
}

// 取调用栈；若回调未注册则返回空。
std::vector<CallStackEntry> DebugController::getCallStack() const {
    return callStackCallback_ ? callStackCallback_() : std::vector<CallStackEntry>{};
}

// ── State queries ──────────────────────────────────────────────────

// 是否处于运行态（running_ 为真表示尚未被 stop）。
bool DebugController::isRunning() const { return running_; }
// 此 headless stub 永不真正阻塞暂停，恒返回 false。
bool DebugController::isPaused() const { return false; }  // never actually pauses

// 复位全部调试状态：模式、深度、断点命中记录、快照与交叉帧标志一并清零，回到初始态。
void DebugController::reset() {
    mode_ = StepMode::MODE_RUN;
    currentDepth_ = 0;
    stepOverDepth_ = 0;
    stepOutDepth_ = 0;
    lastPausedLine_ = -1;
    lastPausedDepth_ = -1;
    running_ = false;
    stopped_ = false;
    // BUG-DBG-14 fix: 重置 crossedLine_/crossedDeeper_ 状态
    lastSeenLine_ = -1;
    crossedLine_ = false;
    crossedDeeper_ = false;
    pauseCount_ = 0;
    pauseLines_.clear();
    pauseDepths_.clear();
    pauseEvents_.clear();
}

// ── Test access ────────────────────────────────────────────────────

// 测试访问接口：返回暂停次数、各次暂停行号、深度与完整暂停事件快照。
int DebugController::pauseCount() const { return pauseCount_; }
const std::vector<int>& DebugController::pauseLines() const { return pauseLines_; }
const std::vector<int>& DebugController::pauseDepths() const { return pauseDepths_; }
const std::vector<DebugPauseEvent>& DebugController::pauseEvents() const { return pauseEvents_; }

// 返回所有暂停事件中出现过的最大栈深度（用于校验 STEP_OUT / 嵌套调用深度）。
int DebugController::maxDepthSeen() const {
    int mx = 0;
    for (int d : pauseDepths_) mx = std::max(mx, d);
    return mx;
}
