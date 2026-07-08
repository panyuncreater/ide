#pragma once

// ============================================================
// VmStepper — VM 单步执行状态机（ARCH-11 拆分自 IdeController）
// ------------------------------------------------------------
// 职责：
//   - 持有 VM / RegisterVM 实例及其执行状态
//   - 实现 STEP_IN / STEP_OVER / STEP_OUT / RUN 四种步进语义
//   - RUN 模式通过 QTimer 异步分批执行，避免主线程冻结
//   - 断点命中检测
//
// A1 fix: 双后端支持。useRegister_=true 时所有操作转发到 regVm_，
// 否则维持原行为转发到栈式 vm_。两个 VM 暴露同构的步进接口
// (initExecution / stepOnce / isFinished / resetState / getCurrentLine /
//  getFrameCount / getLastError / getLastErrorLine)，因此状态机逻辑
// 仅在 initExecution 与状态访问器处分派，其余循环完全复用。
//
// 与 Interpreter DebugController::shouldPauseForStepping 逻辑对齐，
// 但独立实现（VM 是主线程同步执行，不复用 DebugController 跨线程机制）。
// ============================================================

#include <QObject>
#include <QString>
#include <QSet>
#include <QMap>
#include <QTimer>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "compiler/VM.h"
#include "compiler/Bytecode.h"
#include "compiler/RegisterVM.h"  // A1 fix: 寄存器式 VM 后端
#include "compiler/RegisterBytecode.h"
#include "interpreter/Value.h"
#include "debug/DebugTypes.h"  // BUG-DBG-6 fix: CallStackEntry 用于 VM 调用栈显示

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
    // A1 fix: 同时为两个后端设置回调，避免切换后端时回调缺失。
    void setOutputCallback(std::function<void(const std::string&)> cb) {
        auto cbCopy = cb;
        vm_.setOutputCallback(cb);
        regVm_.setOutputCallback(std::move(cbCopy));
    }
    void setInputCallback(std::function<std::string(const std::string&)> cb) {
        auto cbCopy = cb;
        vm_.setInputCallback(cb);
        regVm_.setInputCallback(std::move(cbCopy));
    }

    // ---- 编译结果与断点 ----
    /// 设置栈式 VM 编译结果（步进前必须调用，用于 initExecution）
    // P1-6 fix: 原存储裸指针，编译器状态变更（重新编译/移动 PipelineRunner）后可能悬垂。
    // 改为按值拷贝（std::optional），VmStepper 持有独立所有权，与 Compiler 生命周期解耦。
    void setCompileResult(const CompileResult& result) { lastCompileResult_ = result; }
    /// A1 fix: 设置 RegisterVM 编译结果（仅 useRegister_=true 时使用）
    void setRegisterCompileResult(const RegisterCompileResult& result) {
        lastRegCompileResult_ = result;
    }
    /// A4 fix: 设置 VM 模式断点（复用 Interpreter 的 breakpoint 行号集合）
    void setBreakpoints(const QSet<int>& breakpoints) { vmBreakpoints_ = breakpoints; }

    /// #4 fix: 设置 VM 模式条件断点（行号→条件表达式）
    // BUG-DBG-AUDIT-2 fix: 条件变更时重置对应行的 hitCount（对齐
    // DebugController::setBreakpointCondition 行 308-313，条件变更 → hitCount 清零）。
    void setBreakpointConditions(const QMap<int, std::string>& conditions) {
        vmBreakpointConditions_ = conditions;
        for (auto it = conditions.begin(); it != conditions.end(); ++it) {
            vmBreakpointHitCounts_.remove(it.key());
        }
    }

    /// BUG-DBG-AUDIT-2 fix: 获取 VM 模式断点命中次数。
    /// 对齐 DebugController::getBreakpointHitCount 语义——返回 vmBreakpointHitCounts_[line]，
    /// 不存在时返回 0。仅 VM 模式活跃时由 IdeController::getBreakpointHitCount 分派调用。
    int getBreakpointHitCount(int line) const {
        auto it = vmBreakpointHitCounts_.find(line);
        return it != vmBreakpointHitCounts_.end() ? it.value() : 0;
    }

    /// #4 fix: 设置条件求值器回调（由 IdeController 注入，使用临时 Interpreter + VM 全局变量求值）
    void setConditionEvaluator(std::function<bool(const std::string&)> evaluator) {
        vmConditionEvaluator_ = std::move(evaluator);
    }

    // ---- A1 fix: 后端模式切换 ----
    /// 启用/禁用 RegisterVM 后端。true 时所有步进/状态访问转发到 regVm_。
    /// 切换时自动 reset 两个后端，避免遗留状态污染。
    void setUseRegister(bool enabled) {
        if (useRegister_ == enabled) return;
        // BUG-EXTRA-1 fix: 必须始终调用 reset() 而非仅在 isVmRunning_ 时调用 stop()。
        // VM 暂停时（isVmRunning_=false, isVmInitialized_=true）frame.chunk 指向旧编译结果，
        // 下方 reset() 会释放编译结果导致悬垂指针。reset() 同时清理 isVmInitialized_。
        reset();
        // 清空编译结果：切换后端后旧 CompileResult/RegisterCompileResult
        // 可能与新后端不匹配，强制下次 step 前必须重新编译同步。
        lastCompileResult_.reset();
        lastRegCompileResult_.reset();
        useRegister_ = enabled;
    }
    bool isRegisterMode() const { return useRegister_; }

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
    // B2 fix: 停止 RUN 模式定时器并重置 isVmRunning_，避免 runBatch 在已 resetState 的 VM 上调用
    // currentFrame() 触发 std::abort。原 reset() 遗漏定时器停止 + 状态复位。
    void reset() {
        if (vmRunTimer_) vmRunTimer_->stop();
        isVmRunning_ = false;
        // A1 fix: 双后端都重置，确保切换后端时状态干净
        vm_.resetState();
        regVm_.resetState();
        isVmInitialized_ = false;
        vmStepMode_ = VmStepMode::STEP_IN;
        // BUG-DBG-2 fix: 同步重置 crossedLine_/crossedDeeper_ 状态
        vmLastSeenLine_ = -1;
        vmCrossedLine_ = false;
        vmCrossedDeeper_ = false;
        // BUG-DBG-AUDIT-2 fix: 清空断点命中计数（对齐 DebugController::reset 行 485，
        // 重置所有断点 hitCount，保留断点和条件本身）。
        vmBreakpointHitCounts_.clear();
    }

    bool isRunning() const { return isVmRunning_; }
    bool isInitialized() const { return isVmInitialized_; }

    // ---- VM 状态访问器（B6 fix: 语义化快照接口，GUI 仅通过这些方法读取 VM 状态）----
    // B6 bug fix: getStack 改返回 by value，避免返回 vm_.stack_ 引用导致调用方
    // 缓存引用后步进 VM 触发悬垂/use-after-free
    // A1 fix: 栈式 VM 返回操作数栈；RegisterVM 返回寄存器窗口（同样以 vector<Value> 形式）
    std::vector<Value> getStack() const {
        return useRegister_ ? regVm_.getRegisters() : vm_.getStack();
    }
    std::unordered_map<std::string, Value> getGlobals() const {
        return useRegister_ ? regVm_.getGlobals() : vm_.getGlobalsRef();
    }
    /// BUG-IDE-12 fix: 获取当前帧的局部变量名→值映射（用于 VM 条件断点求值）。
    /// 分派到当前激活后端的 getCurrentFrameLocals()。空帧/主程序帧返回空映射。
    std::unordered_map<std::string, Value> getCurrentFrameLocals() const {
        return useRegister_ ? regVm_.getCurrentFrameLocals() : vm_.getCurrentFrameLocals();
    }
    size_t getCurrentIP() const {
        return useRegister_ ? regVm_.getCurrentIP() : vm_.getCurrentIP();
    }
    // A1 fix: 统一返回操作码名称字符串，兼容 OpCode/RegOp
    std::string getCurrentOpCodeName() const {
        return useRegister_ ? std::string(regOpName(regVm_.getCurrentOpCode()))
                            : std::string(opCodeName(vm_.getCurrentOpCode()));
    }
    int getCurrentLine() const {
        return useRegister_ ? regVm_.getCurrentLine() : vm_.getCurrentLine();
    }
    std::string getCurrentChunkName() const {
        return useRegister_ ? regVm_.getCurrentChunkName() : vm_.getCurrentChunkName();
    }
    std::string getLastError() const {
        return useRegister_ ? regVm_.getLastError() : vm_.getLastError();
    }
    int getLastErrorLine() const {
        return useRegister_ ? regVm_.getLastErrorLine() : vm_.getLastErrorLine();
    }
    // A4 fix: 暴露 VM 调用栈深度（用于 step-over/out 判断）
    size_t getFrameCount() const {
        return useRegister_ ? regVm_.getFrameCount() : vm_.getFrameCount();
    }
    // BUG-DBG-6 fix: 暴露 VM 调用栈快照（用于 GUI 调用栈面板显示）。
    // 原实现 VmStepper 未转发 VM::getCallStack()/RegisterVM::getCallStack()，
    // 导致 VM 模式调试时 DebugPanel 调用栈列表永远空白（数据源是 Interpreter 的空 callStack_）。
    // 转换 VMCallStackEntry/RegCallStackEntry → CallStackEntry：
    //   functionName → functionName
    //   line → line
    //   depth → 帧索引（0=栈底 main，递增到栈顶）
    //   locals → 空（VM 帧无 Environment*，局部变量需从槽位反查，暂不支持）
    std::vector<CallStackEntry> getCallStack() const {
        std::vector<CallStackEntry> result;
        if (useRegister_) {
            auto frames = regVm_.getCallStack();
            result.reserve(frames.size());
            int depth = 0;
            for (const auto& f : frames) {
                CallStackEntry entry;
                entry.functionName = f.functionName;
                entry.line = f.line;
                entry.depth = depth++;
                // locals 留空：VM 帧无 Environment，局部变量需从槽位反查
                result.push_back(std::move(entry));
            }
        } else {
            auto frames = vm_.getCallStack();
            result.reserve(frames.size());
            int depth = 0;
            for (const auto& f : frames) {
                CallStackEntry entry;
                entry.functionName = f.functionName;
                entry.line = f.line;
                entry.depth = depth++;
                result.push_back(std::move(entry));
            }
        }
        return result;
    }

signals:
    // QT-R-01 fix: RUN 模式异步执行暂停时发射，UI 连接到 handleVmStepResult
    void vmRunPaused(VmStepResult result);

private slots:
    /// QT-R-01 fix: RUN 模式定时器回调，每批执行有限步数后让出控制权给事件循环
    void runBatch();

private:
    // A1 fix: 双后端实例。RegisterVM 在 useRegister_=false 时闲置，
    // 不持有运行时资源（resetState 后 frames_/globals_ 均空），内存开销可忽略。
    VM vm_;
    RegisterVM regVm_;
    // P1-6 fix: 原 const T* 裸指针，编译器状态变更/移动可能悬垂。改为按值拷贝持有。
    std::optional<CompileResult> lastCompileResult_;
    std::optional<RegisterCompileResult> lastRegCompileResult_;  // A1 fix

    // ---- VM 步进状态 ----
    // IDE-ATOMIC-01 fix: isVmRunning_ 可能被 IdeController::isVmRunning() 跨线程查询
    // （REPL 异步任务 / Worker 回调），改为 atomic<bool> 消除数据竞争 UB。
    // isVmInitialized_ / useRegister_ 仅在主线程访问（QTimer + UI 槽），保持普通 bool。
    std::atomic<bool> isVmRunning_{false};
    bool isVmInitialized_ = false;
    // A1 fix: 后端选择标志。false=栈式 VM（默认），true=RegisterVM
    bool useRegister_ = false;
    // A4 fix: VM 步进状态机（轻量版，不依赖 DebugController 的跨线程机制）
    VmStepMode vmStepMode_ = VmStepMode::STEP_IN;
    size_t vmStepStartFrameCount_ = 0;  // step-over/out 起始帧深度
    int vmLastPausedLine_ = 0;          // 上次暂停的行号（防同行重复触发）
    // AUDIT-BUG-D2 fix: STEP_OVER 期间是否进入过更深的帧。
    // 与 Interpreter DebugController::crossedDeeper_ 对齐——
    // 同行函数调用返回后即使行号不变也应暂停。
    bool vmCrossedDeeper_ = false;
    // BUG-DBG-2 fix: crossedLine 机制，与 DebugController::crossedLine_ 对齐。
    // 单行循环断点（如 for (...; ...; ...) print(i);）在 RUN 模式下需每次迭代重新触发。
    // 原实现仅用 currentLine != vmLastPausedLine_ 去重，导致首次命中后永不再触发。
    int vmLastSeenLine_ = -1;          // 上次见到的行号
    bool vmCrossedLine_ = false;        // 是否跨过不同行（允许同行断点重新触发）
    QSet<int> vmBreakpoints_;          // VM 模式断点行号集合（复用 Editor 断点）
    QMap<int, std::string> vmBreakpointConditions_;  // #4 fix: 条件断点表达式
    // BUG-DBG-AUDIT-2 fix: VM 模式断点命中计数（行号→次数），对齐
    // DebugController::breakpointInfos_[line].hitCount。checkBreakpointHit 命中时递增，
    // reset() 清空，setBreakpointConditions 重置对应行。
    QMap<int, int> vmBreakpointHitCounts_;
    std::function<bool(const std::string&)> vmConditionEvaluator_;  // #4 fix: 条件求值回调
    // QT-R-01 fix: RUN 模式异步分批执行的定时器
    QTimer* vmRunTimer_ = nullptr;
    int64_t vmRunStepCount_ = 0;        // RUN 模式累计执行步数（用于总量上限保护）

    // ---- A1 fix: 单步执行分派辅助 ----
    // 两个 VM 的 stepOnce 均返回 VMResult，无需 vtable，直接 if 分派更高效
    VMResult stepOnceActive();
    bool isActiveFinished() const;
    bool initActiveExecution();  // P1-5 fix: 返回 false 表示编译结果为空，caller 不应标记 initialized
    void resetActiveState();

    /// #4 fix: 检查断点命中（含条件求值）。返回 true 表示应在此行暂停。
    /// line: 当前 IP 所在行号。若该行有条件断点，调用 evaluator 求值；
    /// 无条件或求值为真时返回 true。
    bool checkBreakpointHit(int line);
};
