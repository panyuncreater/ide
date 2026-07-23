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

    // 步进模式控制：stepIn/stepOver/stepOut/resume/stop 设置各调试模式，setCurrentDepth 由解释器同步当前栈深度。
    void stepIn();
    void stepOver();
    void stepOut();
    void resume();
    void stop();
    void setCurrentDepth(int depth);

    // checkBreak：解释器在执行每个 AST 节点前调用，本桩据此判定是否记录一次暂停事件。
    void checkBreak(ASTNode* node);

    // 断点管理：增删查断点、批量设置、为断点附加条件表达式及条件求值器。
    void setBreakpoint(int line);
    void removeBreakpoint(int line);
    bool hasBreakpoint(int line) const;
    void setBreakpoints(const std::set<int>& lines);
    void setBreakpointCondition(int line, const std::string& cond);
    void setConditionEvaluator(std::function<bool(const std::string&)> eval);

    // R104 Logpoint：日志断点 API（与生产版 DebugController 同步）。
    // 桩实现仅记录状态，不实际输出日志（无 UI 通道）。
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setBreakpointKind(int line, BreakpointKind kind);
    BreakpointKind getBreakpointKind(int line) const;
    void setLogpointMessage(int line, const std::string& msg);
    std::string getLogpointMessage(int line) const;

    // R104 Function Breakpoint：函数断点 API（与生产版同步）。
    // 桩实现仅记录状态，checkFunctionBreakpoint 在测试中可被手动调用。
    void setFunctionBreakpoint(const std::string& functionName);
    void removeFunctionBreakpoint(const std::string& functionName);
    void setFunctionBreakpoints(const std::set<std::string>& names);
    std::set<std::string> getFunctionBreakpoints() const;
    void setFunctionBreakpointCondition(const std::string& functionName, const std::string& cond);
    std::string getFunctionBreakpointCondition(const std::string& functionName) const;
    int getFunctionBreakpointHitCount(const std::string& functionName) const;
    bool hasFunctionBreakpoint(const std::string& functionName) const;
    bool checkFunctionBreakpoint(const std::string& functionName, int line);

    // R104 Exception Breakpoint：异常断点 API（与生产版同步）。
    void setExceptionBreakpointEnabled(bool enabled);
    bool isExceptionBreakpointEnabled() const;
    int getExceptionBreakpointHitCount() const;
    bool checkExceptionBreakpoint(int line);

    // 回调注册：变量快照与调用栈采集回调，供暂停时记录上下文。
    void setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb);
    void setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb);
    std::vector<VariableSnapshot> getVariableSnapshot() const;
    std::vector<CallStackEntry> getCallStack() const;

    // 状态查询：运行态/暂停态以及整体复位。
    bool isRunning() const;
    bool isPaused() const;
    void reset();

    // 测试访问接口：暴露累计的暂停次数、行号、深度与完整事件快照，供测试断言。
    int pauseCount() const;
    const std::vector<int>& pauseLines() const;
    const std::vector<int>& pauseDepths() const;
    const std::vector<DebugPauseEvent>& pauseEvents() const;
    int maxDepthSeen() const;

    // R104 测试访问：暴露 Logpoint 日志记录与命中次数（仅桩实现提供）。
    const std::vector<std::pair<int, std::string>>& logpointLogs() const;
    int logpointHitCount(int line) const;

private:
    // 每实例调试状态：当前模式、断点集合、条件、栈深度跟踪与各帧/行号去重标志。
    StepMode mode_ = StepMode::MODE_RUN;
    std::set<int> breakpoints_;
    std::map<int, BreakpointInfo> breakpointInfos_;
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

    // R104 新断点存储
    std::function<void(const std::string&)> logCallback_;
    std::map<std::string, FunctionBreakpointInfo> functionBreakpoints_;
    ExceptionBreakpointState exceptionBreakpoint_;
    // 测试访问：记录 Logpoint 命中事件（行号, 消息）
    std::vector<std::pair<int, std::string>> logpointLogs_;
};
