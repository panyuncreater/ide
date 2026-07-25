#pragma once

// ============================================================
// DebugCoordinator — 调试状态协调（ARCH-11 拆分自 IdeController）
// ------------------------------------------------------------
// 职责：
//   - 设置断点及条件表达式
//   - 配置调试器回调（条件求值、变量快照、调用栈快照）
//   - 步进控制（stepIn / stepOver / stepOut / resume / stop）
//   - 调试状态查询（hasBreakpoints / getDebugVariableSnapshot / getDebugCallStack）
//
// 与 WorkerManager 共享 Interpreter / DebugController 所有权。
// ============================================================

#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/Logger.h"
#include "debug/DebugController.h"
#include "interpreter/Interpreter.h"

class DebugCoordinator : public QObject {
    Q_OBJECT

public:
    DebugCoordinator(std::shared_ptr<Interpreter> interpreter, std::shared_ptr<DebugController> debugger,
                     QObject* parent = nullptr);

    // #10 fix: 析构时反注册 debugger_ 上的回调。debugger_ 与 WorkerManager 共享所有权，
    // 可能比 DebugCoordinator 存活更久；setupDebug 注册的三个 lambda 捕获裸 this，
    // 若不反注册，异常关闭路径下 worker 线程调用这些回调将触发 UAF。
    ~DebugCoordinator() override;

    // ---- 断点管理 ----
    /// 设置断点及条件表达式（GUI-03 fix: 使用 evaluateCondition 安全求值条件断点）
    void setupDebug(const QSet<int>& breakpoints, const QMap<int, std::string>& conditions);
    void setBreakpoints(const QSet<int>& breakpoints) { debugger_->setBreakpoints(breakpoints); }
    void setBreakpointCondition(int line, const std::string& condition) {
        debugger_->setBreakpointCondition(line, condition);
    }
    bool hasBreakpoints() const { return !debugger_->getBreakpoints().isEmpty(); }

    // P1-2: 断点查询接口（供 BreakpointConditionPanel 消费）
    QSet<int> getBreakpoints() const { return debugger_->getBreakpoints(); }
    std::string getBreakpointCondition(int line) const { return debugger_->getBreakpointCondition(line); }
    int getBreakpointHitCount(int line) const { return debugger_->getBreakpointHitCount(line); }

    // ---- 步进控制 ----
    void stepIn() { debugger_->stepIn(); }
    void stepOver() { debugger_->stepOver(); }
    void stepOut() { debugger_->stepOut(); }
    void resume() { debugger_->resume(); }
    void stop() { debugger_->stop(); }

    // R98 runToCursor: 一次性临时断点转发。Interpreter 路径 runToCursor 通过此接口
    // 设置临时断点后调用 resume()，临时断点命中后由 DebugController::checkBreak 自动清除。
    void setTemporaryBreakpoint(int line) { debugger_->setTemporaryBreakpoint(line); }
    void clearTemporaryBreakpoint() { debugger_->clearTemporaryBreakpoint(); }
    int getTemporaryBreakpoint() const { return debugger_->getTemporaryBreakpoint(); }

    // ---- R104 调试器拓展：Logpoint / Function BP / Exception BP ----
    // 转发到 DebugController（Interpreter 路径）。VM 路径通过 VmStepper 独立实现。
    void setLogCallback(std::function<void(const std::string&)> cb) { debugger_->setLogCallback(std::move(cb)); }
    void setBreakpointKind(int line, BreakpointKind kind) { debugger_->setBreakpointKind(line, kind); }
    BreakpointKind getBreakpointKind(int line) const { return debugger_->getBreakpointKind(line); }
    void setLogpointMessage(int line, const std::string& msg) { debugger_->setLogpointMessage(line, msg); }
    std::string getLogpointMessage(int line) const { return debugger_->getLogpointMessage(line); }

    void setFunctionBreakpoint(const std::string& name) { debugger_->setFunctionBreakpoint(name); }
    void removeFunctionBreakpoint(const std::string& name) { debugger_->removeFunctionBreakpoint(name); }
    void setFunctionBreakpoints(const QSet<std::string>& names) { debugger_->setFunctionBreakpoints(names); }
    QSet<std::string> getFunctionBreakpoints() const { return debugger_->getFunctionBreakpoints(); }
    void setFunctionBreakpointCondition(const std::string& name, const std::string& cond) {
        debugger_->setFunctionBreakpointCondition(name, cond);
    }
    std::string getFunctionBreakpointCondition(const std::string& name) const {
        return debugger_->getFunctionBreakpointCondition(name);
    }
    int getFunctionBreakpointHitCount(const std::string& name) const {
        return debugger_->getFunctionBreakpointHitCount(name);
    }
    bool hasFunctionBreakpoint(const std::string& name) const { return debugger_->hasFunctionBreakpoint(name); }

    void setExceptionBreakpointEnabled(bool enabled) { debugger_->setExceptionBreakpointEnabled(enabled); }
    bool isExceptionBreakpointEnabled() const { return debugger_->isExceptionBreakpointEnabled(); }
    int getExceptionBreakpointHitCount() const { return debugger_->getExceptionBreakpointHitCount(); }

    // L19 Watchpoint：转发到 DebugController（Interpreter 路径）。
    // IdeController facade 同时设置 vmStepper_ 和 debugCoord_，确保路径切换后 watchpoint 不丢失。
    void setWatchpoint(const WatchpointInfo& wp) { debugger_->setWatchpoint(wp); }
    void removeWatchpoint(const std::string& varName, const std::string& fieldName = "") {
        debugger_->removeWatchpoint(varName, fieldName);
    }
    void clearWatchpoints() { debugger_->clearWatchpoints(); }
    std::vector<WatchpointInfo> getWatchpoints() const { return debugger_->getWatchpoints(); }
    bool hasWatchpoints() const { return debugger_->hasWatchpoints(); }

    // ---- 调试状态查询 ----
    std::vector<VariableSnapshot> getDebugVariableSnapshot() const { return debugger_->getVariableSnapshot(); }
    std::vector<CallStackEntry> getDebugCallStack() const { return debugger_->getCallStack(); }
    /// AUDIT-P1 fix: 转发 DebugController::isPaused()，供 IdeController::isDebugPaused() 使用。
    /// 面板 autoTimer 在调试 resume 期间调用快照接口会与 worker 线程并发访问解释器内部数据。
    bool isPaused() const { return debugger_->isPaused(); }

    /// R121 调用栈帧切换：按帧索引获取该帧的局部变量（Interpreter 路径）。
    /// @param depth 帧索引（0=栈底 main，递增到栈顶）。越界或无 env 返回空。
    /// @note 通过 interpreter_->getCallStackSnapshot() 直接访问 CallFrame.env->snapshotLocalVariables()，
    ///       避免每次调用都通过 debugger_->getCallStack() 触发完整 callback 重建栈快照。
    /// @note R121-build fix: 改为 inline 以避免 VariableInspectorPanel.cpp 依赖
    ///       DebugCoordinator.cpp 的非 inline 符号（minilang_tests 不链接 DebugCoordinator.cpp）。
    ///       与 R117 WatchExpressionLibrary 抽取 Library.cpp 的解法不同——此处逻辑简单且
    ///       DebugCoordinator.h 已传递包含 Interpreter.h / CallFrame.h / Environment.h。
    std::vector<std::pair<std::string, Value>> getFrameLocalsAt(int depth) const {
        std::vector<std::pair<std::string, Value>> result;
        try {
            auto stack = interpreter_->getCallStackSnapshot();
            if (depth < 0 || depth >= static_cast<int>(stack.size())) {
                return result;
            }
            const auto& frame = stack[depth];
            if (frame.env) {
                // P0-2 fix: 用 snapshotLocalVariables() 获取值拷贝，避免直接遍历
                // Environment::variables 的 const 引用（worker 线程可能并发修改）。
                auto locals = frame.env->snapshotLocalVariables();
                for (const auto& kv : locals) {
                    result.emplace_back(kv.first, kv.second);
                }
            }
        } catch (const std::exception& e) {
            Logger::Warning(std::string("getFrameLocalsAt 异常: ") + e.what(), "Debugger");
        } catch (...) {
            Logger::Warning("getFrameLocalsAt 发生未知异常", "Debugger");
        }
        return result;
    }

signals:
    /// 转发调试器暂停信号
    void pausedAt(int line);

private:
    std::shared_ptr<Interpreter> interpreter_;
    std::shared_ptr<DebugController> debugger_;
};
