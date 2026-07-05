// ============================================================
// DebugController — headless (non-Qt) stub for test_harness
// ------------------------------------------------------------
// 无 Qt 依赖的调试控制器桩，实现步进模式逻辑、记录暂停事件、
// 自动恢复（不阻塞线程）。Q_OBJECT-free 以避免 AUTOMOC 冲突。
// 类名保持 DebugController 以兼容 Interpreter::setDebugger()。
//
// 修复 LNK2005：实现已移到 debug/DebugController.cpp。
// 作为 .obj 直接链接到 debug_test.exe，优先级高于
// minilang_core.lib 中的同名符号，避免多重定义。
// ============================================================
#pragma once

#include <vector>
#include <string>
#include <functional>
#include <set>
#include <map>
#include "interpreter/Value.h"
#include "ast/ASTNode.h"
#include "debug/DebugTypes.h"

// ── Recorded pause event ──────────────────────────────────────────
struct DebugPauseEvent {
    int line;
    int depth;
    std::vector<std::pair<std::string, std::string>> variables;
    std::vector<std::string> callStack;
};

// ── DebugController (headless, no Qt) ─────────────────────────────
class DebugController {
public:
    DebugController() = default;
    ~DebugController() = default;

    // Step mode control
    void stepIn();
    void stepOver();
    void stepOut();
    void resume();
    void stop();
    void setCurrentDepth(int depth);

    // checkBreak: called by Interpreter before every AST node
    void checkBreak(ASTNode* node);

    // Breakpoint management
    void setBreakpoint(int line);
    void removeBreakpoint(int line);
    bool hasBreakpoint(int line) const;
    void setBreakpoints(const std::set<int>& lines);
    void setBreakpointCondition(int line, const std::string& cond);
    void setConditionEvaluator(std::function<bool(const std::string&)> eval);

    // Callbacks
    void setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb);
    void setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb);
    std::vector<VariableSnapshot> getVariableSnapshot() const;
    std::vector<CallStackEntry> getCallStack() const;

    // State queries
    bool isRunning() const;
    bool isPaused() const;
    void reset();

    // Test access: recorded data
    int pauseCount() const;
    const std::vector<int>& pauseLines() const;
    const std::vector<int>& pauseDepths() const;
    const std::vector<DebugPauseEvent>& pauseEvents() const;
    int maxDepthSeen() const;

private:
    StepMode mode_ = StepMode::MODE_RUN;
    std::set<int> breakpoints_;
    std::map<int, std::string> breakpointConditions_;
    std::function<bool(const std::string&)> conditionEvaluator_;
    int currentDepth_ = 0;
    int stepOverDepth_ = 0;
    int stepOutDepth_ = 0;
    int lastPausedLine_ = -1;
    int lastPausedDepth_ = -1;
    bool running_ = false;
    bool stopped_ = false;
    // BUG-DBG-14 fix: 补齐与真实 DebugController 对齐的关键修复机制
    // crossedLine_: 单行循环断点重新触发（F4 fix）
    int lastSeenLine_ = -1;
    bool crossedLine_ = false;
    // crossedDeeper_: STEP_OVER 从更深帧返回后即使同行也暂停（DBG-B fix）
    bool crossedDeeper_ = false;

    // Recording
    int pauseCount_ = 0;
    std::vector<int> pauseLines_;
    std::vector<int> pauseDepths_;
    std::vector<DebugPauseEvent> pauseEvents_;

    std::function<std::vector<VariableSnapshot>()> variableCallback_;
    std::function<std::vector<CallStackEntry>()> callStackCallback_;
};
