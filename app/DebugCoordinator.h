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

#include <QObject>
#include <QString>
#include <QSet>
#include <QMap>
#include <memory>
#include <string>
#include <vector>

#include "interpreter/Interpreter.h"
#include "debug/DebugController.h"

class DebugCoordinator : public QObject {
    Q_OBJECT

public:
    DebugCoordinator(std::shared_ptr<Interpreter> interpreter,
                     std::shared_ptr<DebugController> debugger,
                     QObject* parent = nullptr);

    // #10 fix: 析构时反注册 debugger_ 上的回调。debugger_ 与 WorkerManager 共享所有权，
    // 可能比 DebugCoordinator 存活更久；setupDebug 注册的三个 lambda 捕获裸 this，
    // 若不反注册，异常关闭路径下 worker 线程调用这些回调将触发 UAF。
    ~DebugCoordinator() override;

    // ---- 断点管理 ----
    /// 设置断点及条件表达式（GUI-03 fix: 使用 evaluateCondition 安全求值条件断点）
    void setupDebug(const QSet<int>& breakpoints,
                    const QMap<int, std::string>& conditions);
    void setBreakpoints(const QSet<int>& breakpoints) {
        debugger_->setBreakpoints(breakpoints);
    }
    void setBreakpointCondition(int line, const std::string& condition) {
        debugger_->setBreakpointCondition(line, condition);
    }
    bool hasBreakpoints() const { return !debugger_->getBreakpoints().isEmpty(); }

    // P1-2: 断点查询接口（供 BreakpointConditionPanel 消费）
    QSet<int> getBreakpoints() const { return debugger_->getBreakpoints(); }
    std::string getBreakpointCondition(int line) const {
        return debugger_->getBreakpointCondition(line);
    }
    int getBreakpointHitCount(int line) const {
        return debugger_->getBreakpointHitCount(line);
    }

    // ---- 步进控制 ----
    void stepIn()  { debugger_->stepIn(); }
    void stepOver() { debugger_->stepOver(); }
    void stepOut() { debugger_->stepOut(); }
    void resume()  { debugger_->resume(); }
    void stop()    { debugger_->stop(); }

    // ---- 调试状态查询 ----
    std::vector<VariableSnapshot> getDebugVariableSnapshot() const {
        return debugger_->getVariableSnapshot();
    }
    std::vector<CallStackEntry> getDebugCallStack() const {
        return debugger_->getCallStack();
    }

signals:
    /// 转发调试器暂停信号
    void pausedAt(int line);

private:
    std::shared_ptr<Interpreter> interpreter_;
    std::shared_ptr<DebugController> debugger_;
};
