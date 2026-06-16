#pragma once

#include <QObject>
#include <QSet>
#include <QMap>
#include <QEventLoop>
#include <vector>
#include <string>
#include <functional>
#include <utility>
#include "interpreter/Value.h"

// ============================================================
// DebugController 调试控制器
// ============================================================

/// 调试步进模式
enum class StepMode {
    MODE_RUN,       // 正常运行（仅检查断点）
    MODE_STEP_IN,   // 单步进入（每个节点暂停）
    MODE_STEP_OVER, // 单步跳过（同调用深度暂停）
    MODE_STEP_OUT,  // 单步跳出（浅于当前深度时暂停）
    MODE_PAUSE      // 暂停
};

/// 变量快照条目
struct VariableSnapshot {
    std::string name;
    Value value;
    std::string scope;  // 作用域描述
};

/// 调用栈条目
struct CallStackEntry {
    std::string functionName;
    int line;
    int depth;
    std::vector<std::pair<std::string, Value>> locals;  // 该帧的局部变量快照
};

/// 断点信息（支持条件断点）
struct BreakpointInfo {
    int line;
    std::string condition;  // 条件表达式（空字符串 = 无条件断点）
    int hitCount = 0;       // 命中次数

    BreakpointInfo() : line(0) {}
    BreakpointInfo(int ln, const std::string& cond = "")
        : line(ln), condition(cond) {}

    bool isConditional() const { return !condition.empty(); }
};

/// 调试控制器：管理断点、步进模式和暂停
class DebugController : public QObject {
    Q_OBJECT

public:
    explicit DebugController(QObject* parent = nullptr);
    ~DebugController();

    /// 在每个 AST 节点执行前调用
    void checkBreak(class ASTNode* node);

    /// 断点管理
    void setBreakpoint(int line);
    void setBreakpoints(const QSet<int>& lines);
    void removeBreakpoint(int line);
    void toggleBreakpoint(int line);
    bool hasBreakpoint(int line) const;
    QSet<int> getBreakpoints() const;

    /// 条件断点：设置断点条件表达式
    void setBreakpointCondition(int line, const std::string& condition);

    /// 获取断点条件
    std::string getBreakpointCondition(int line) const;

    /// 获取断点命中次数
    int getBreakpointHitCount(int line) const;

    /// 设置条件表达式求值回调（由 IDE 设置，接收条件字符串，返回 bool）
    void setConditionEvaluator(std::function<bool(const std::string&)> evaluator);

    /// 步进控制
    void stepIn();
    void stepOver();
    void stepOut();
    void resume();
    void stop();

    /// 设置当前调用深度（由 Interpreter 更新）
    void setCurrentDepth(int depth);

    /// 设置变量快照回调（由主窗口设置）
    void setVariableCallback(std::function<std::vector<VariableSnapshot>()> cb);

    /// 设置调用栈回调
    void setCallStackCallback(std::function<std::vector<CallStackEntry>()> cb);

    /// 获取变量快照
    std::vector<VariableSnapshot> getVariableSnapshot() const;

    /// 获取调用栈
    std::vector<CallStackEntry> getCallStack() const;

    /// 是否正在运行
    bool isRunning() const;

    /// 是否处于暂停状态
    bool isPaused() const;

    /// 重置状态
    void reset();

signals:
    /// 调试暂停在某行
    void pausedAt(int line);

    /// 调试执行结束
    void executionFinished();

    /// 变量变更通知
    void variablesChanged();

private:
    StepMode mode_ = StepMode::MODE_RUN;
    QSet<int> breakpoints_;     // 断点行号集合
    QMap<int, BreakpointInfo> breakpointInfos_;  // 条件断点详情（行号→信息）
    std::function<bool(const std::string&)> conditionEvaluator_;  // 条件表达式求值器
    int currentDepth_ = 0;      // 当前调用深度
    int stepOverDepth_ = 0;     // stepOver 时的调用深度
    int stepOutDepth_ = 0;      // stepOut 时的调用深度
    int lastPausedLine_ = -1;   // 上次暂停的行号（避免同行重复暂停）
    int minBreakpointLine_ = -1; // 最小断点行号（快速跳过不可能命中的节点）
    bool running_ = false;      // 是否正在运行
    bool stopped_ = false;      // 是否被停止
    bool paused_ = false;       // 是否处于暂停状态（等待用户操作）
    bool inPauseLoop_ = false;  // 是否正在暂停事件循环中（防重入）
    QEventLoop* pauseLoop_ = nullptr;  // 暂停时的事件循环（替代忙等）

    std::function<std::vector<VariableSnapshot>()> variableCallback_;
    std::function<std::vector<CallStackEntry>()> callStackCallback_;

    /// 暂停当前线程，等待用户操作
    void pauseExecution();

    /// 更新最小断点行号缓存
    void updateMinBreakpointLine();
};
