#pragma once

// ============================================================
// VmStepper — VM 单步执行状态机（ARCH-11 拆分自 IdeController）
// ------------------------------------------------------------
// 职责：
//   - 持有 VM 实例及其执行状态
//   - 实现 STEP_IN / STEP_OVER / STEP_OUT / RUN 四种步进语义
//   - RUN 模式通过 QTimer 异步分批执行，避免主线程冻结
//   - 断点命中检测
//
// 与 Interpreter DebugController::shouldPauseForStepping 逻辑对齐，
// 但独立实现（VM 是主线程同步执行，不复用 DebugController 跨线程机制）。
// ============================================================

#include <QObject>
#include <QString>
#include <QSet>
#include <QTimer>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "compiler/VM.h"
#include "compiler/Bytecode.h"
#include "interpreter/Value.h"

class VmStepper : public QObject {
    Q_OBJECT

public:
    // QT-R-01 fix: RUNNING 表示 RUN 模式已异步启动，等待 vmRunPaused 信号
    enum class VmStepResult { OK, FINISHED, ERROR, NOT_READY, PAUSED_AT_BREAKPOINT, RUNNING };
    /// A4 fix: VM 步进模式（与 Interpreter DebugController::StepMode 对齐）
    enum class VmStepMode { STEP_IN, STEP_OVER, STEP_OUT, RUN };

    explicit VmStepper(QObject* parent = nullptr);
    ~VmStepper();

    // ---- 回调设置（由 IdeController 在构造时调用）----
    void setOutputCallback(std::function<void(const std::string&)> cb) {
        vm_.setOutputCallback(std::move(cb));
    }
    void setInputCallback(std::function<std::string(const std::string&)> cb) {
        vm_.setInputCallback(std::move(cb));
    }

    // ---- 编译结果与断点 ----
    /// 设置编译结果（步进前必须调用，用于 initExecution）
    void setCompileResult(const CompileResult& result) { lastCompileResult_ = &result; }
    /// A4 fix: 设置 VM 模式断点（复用 Interpreter 的 breakpoint 行号集合）
    void setBreakpoints(const QSet<int>& breakpoints) { vmBreakpoints_ = breakpoints; }

    // ---- 步进操作 ----
    /// 单步执行 VM（等价于 stepIn），返回结果状态
    VmStepResult step();

    /// A4 fix: 按指定模式执行 VM 步进
    /// - STEP_IN: 执行一条指令即返回（同步）
    /// - STEP_OVER: 执行直到帧深度 <= 起始深度且行号变化（同步）
    /// - STEP_OUT: 执行直到帧深度 < 起始深度（同步）
    /// - RUN: 全速执行直到命中断点/结束/错误
    ///   QT-R-01 fix: RUN 模式改为异步 — 启动 QTimer 分批执行，立即返回 RUNNING。
    ///   执行暂停时通过 vmRunPaused 信号通知 UI。
    VmStepResult stepByMode(VmStepMode mode);

    /// A4 fix: 停止 VM 并重置状态
    void stop();
    /// A4 fix: 重置 VM 状态（用于重新开始）
    void reset() { vm_.resetState(); isVmInitialized_ = false; vmStepMode_ = VmStepMode::STEP_IN; }

    bool isRunning() const { return isVmRunning_; }
    bool isInitialized() const { return isVmInitialized_; }

    // ---- VM 状态访问器（B6 fix: 语义化快照接口，GUI 仅通过这些方法读取 VM 状态）----
    // B6 bug fix: getStack 改返回 by value，避免返回 vm_.stack_ 引用导致调用方
    // 缓存引用后步进 VM 触发悬垂/use-after-free
    std::vector<Value> getStack() const { return vm_.getStack(); }
    std::unordered_map<std::string, Value> getGlobals() const { return vm_.getGlobalsRef(); }
    size_t getCurrentIP() const { return vm_.getCurrentIP(); }
    OpCode getCurrentOpCode() const { return vm_.getCurrentOpCode(); }
    int getCurrentLine() const { return vm_.getCurrentLine(); }
    std::string getCurrentChunkName() const { return vm_.getCurrentChunkName(); }
    std::string getLastError() const { return vm_.getLastError(); }
    int getLastErrorLine() const { return vm_.getLastErrorLine(); }
    // A4 fix: 暴露 VM 调用栈深度（用于 step-over/out 判断）
    size_t getFrameCount() const { return vm_.getFrameCount(); }

signals:
    // QT-R-01 fix: RUN 模式异步执行暂停时发射，UI 连接到 handleVmStepResult
    void vmRunPaused(VmStepResult result);

private slots:
    /// QT-R-01 fix: RUN 模式定时器回调，每批执行有限步数后让出控制权给事件循环
    void runBatch();

private:
    VM vm_;
    const CompileResult* lastCompileResult_ = nullptr;

    // ---- VM 步进状态 ----
    bool isVmRunning_ = false;
    bool isVmInitialized_ = false;
    // A4 fix: VM 步进状态机（轻量版，不依赖 DebugController 的跨线程机制）
    VmStepMode vmStepMode_ = VmStepMode::STEP_IN;
    size_t vmStepStartFrameCount_ = 0;  // step-over/out 起始帧深度
    int vmLastPausedLine_ = 0;          // 上次暂停的行号（防同行重复触发）
    QSet<int> vmBreakpoints_;          // VM 模式断点行号集合（复用 Editor 断点）
    // QT-R-01 fix: RUN 模式异步分批执行的定时器
    QTimer* vmRunTimer_ = nullptr;
    int64_t vmRunStepCount_ = 0;        // RUN 模式累计执行步数（用于总量上限保护）
};
