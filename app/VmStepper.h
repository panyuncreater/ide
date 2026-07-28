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

#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread> // AUDIT-R4 BUG-14: assertMainThread 线程亲和性断言
#include <QTimer>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/IDebugController.h" // P1-4: 调试控制器统一接口
#include "compiler/Bytecode.h"
#include "compiler/RegisterBytecode.h"
#include "compiler/RegisterVM.h" // A1 fix: 寄存器式 VM 后端
#include "compiler/VM.h"
#include "debug/DebugTypes.h"             // BUG-DBG-6 fix: CallStackEntry 用于 VM 调用栈显示
#include "debug/ExecutionTraceRecorder.h" // R114: 可回放执行时间轴 recorder
#include "interpreter/Value.h"

class VmStepper : public QObject, public IDebugController {
    Q_OBJECT

public:
    // QT-R-01 fix: RUNNING 表示 RUN 模式已异步启动，等待 vmRunPaused 信号
    enum class VmStepResult { OK, FINISHED, ERROR, NOT_READY, PAUSED_AT_BREAKPOINT, RUNNING };
    /// A4 fix: VM 步进模式（与 Interpreter DebugController::StepMode 对齐）
    enum class VmStepMode { STEP_IN, STEP_OVER, STEP_OUT, RUN };

    explicit VmStepper(QObject* parent = nullptr);
    ~VmStepper() override;

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
    void setRegisterCompileResult(const RegisterCompileResult& result) { lastRegCompileResult_ = result; }
    /// A4 fix: 设置 VM 模式断点（复用 Interpreter 的 breakpoint 行号集合）
    void setBreakpoints(const QSet<int>& breakpoints) override {
        assertMainThread(); // AUDIT-R4 BUG-14
        vmBreakpoints_ = breakpoints;
    }

    /// R98 runToCursor: 设置一次性临时断点（仅命中一次后自动清除）。
    /// 调用此方法后调用 stepByMode(VmStepMode::RUN) 即可"运行到目标行"。
    /// 与用户断点独立——临时断点命中后自动清除，不影响 vmBreakpoints_。
    /// 已设置未命中的临时断点会被新调用覆盖（取最后一次目标行）。
    /// @param line 目标行号（必须 > 0，否则忽略）
    /// @note stop()/reset()/析构会清除临时断点
    void setTempBreakpoint(int line) {
        assertMainThread(); // AUDIT-R4 BUG-14: 非 atomic 字段仅限主线程访问
        if (line <= 0)
            return;
        vmTempBreakpointLine_ = line;
    }

    /// R98 runToCursor: 清除临时断点（手动取消/停止/重置时调用）。
    /// VmStepper 仅在主线程访问（QTimer + UI 槽），无需加锁。
    void clearTempBreakpoint() {
        assertMainThread(); // AUDIT-R4 BUG-14
        vmTempBreakpointLine_ = -1;
    }

    /// R98 runToCursor: 查询当前临时断点行号（调试/测试用）。
    /// @return 临时断点行号；无临时断点时返回 -1
    int getTempBreakpoint() const {
        assertMainThread(); // AUDIT-R4 BUG-14
        return vmTempBreakpointLine_;
    }

    // P1-4 fix: IDebugController 接口要求的临时断点方法（转发到 setTempBreakpoint 系列别名）
    void setTemporaryBreakpoint(int line) override { setTempBreakpoint(line); }
    void clearTemporaryBreakpoint() override { clearTempBreakpoint(); }
    int getTemporaryBreakpoint() const override { return getTempBreakpoint(); }

    /// #4 fix: 设置 VM 模式条件断点（行号→条件表达式）
    // BUG-DBG-AUDIT-2 fix: 条件变更时重置对应行的 hitCount（对齐
    // DebugController::setBreakpointCondition 行 308-313，条件变更 → hitCount 清零）。
    void setBreakpointConditions(const QMap<int, std::string>& conditions) {
        assertMainThread(); // AUDIT-R4 BUG-14
        vmBreakpointConditions_ = conditions;
        for (auto it = conditions.begin(); it != conditions.end(); ++it) {
            vmBreakpointHitCounts_.remove(it.key());
        }
    }

    /// BUG-DBG-AUDIT-2 fix: 获取 VM 模式断点命中次数。
    /// 对齐 DebugController::getBreakpointHitCount 语义——返回 vmBreakpointHitCounts_[line]，
    /// 不存在时返回 0。仅 VM 模式活跃时由 IdeController::getBreakpointHitCount 分派调用。
    int getBreakpointHitCount(int line) const override {
        auto it = vmBreakpointHitCounts_.find(line);
        return it != vmBreakpointHitCounts_.end() ? it.value() : 0;
    }

    // ---- 拓展二期：命中条件（hit condition）+ 依赖断点链 ----
    /// 设置行断点命中条件（"N"/"== N"/">= N"/"> N"/"% N"，空=清除）。
    /// 变更时重置该行 hitCount（对齐 setBreakpointConditions 语义）。
    void setBreakpointHitCondition(int line, const std::string& expr) {
        assertMainThread();
        if (expr.empty()) {
            vmBreakpointHitConditions_.remove(line);
        } else {
            vmBreakpointHitConditions_[line] = expr;
        }
        vmBreakpointHitCounts_.remove(line);
    }
    std::string getBreakpointHitCondition(int line) const {
        auto it = vmBreakpointHitConditions_.find(line);
        return it != vmBreakpointHitConditions_.end() ? it.value() : std::string{};
    }

    /// 设置依赖断点链：仅当 dependsOnLine 的断点至少命中过一次后，
    /// line 的断点才激活（depLine <= 0 清除依赖）。
    void setBreakpointDependency(int line, int depLine) {
        assertMainThread();
        if (depLine <= 0) {
            vmBreakpointDependencies_.remove(line);
        } else {
            vmBreakpointDependencies_[line] = depLine;
        }
    }
    int getBreakpointDependency(int line) const {
        auto it = vmBreakpointDependencies_.find(line);
        return it != vmBreakpointDependencies_.end() ? it.value() : -1;
    }

    // ---- 拓展二期：调试暂停时写变量（setVariable）----
    /// 先尝试当前栈顶帧局部（含 upvalue），未命中回退全局。
    /// 仅在 VM 非运行（暂停/单步间隙）时允许写入。
    /// @return true 写入成功；false 变量不存在或 VM 正在运行
    bool setVariableValue(const std::string& name, const Value& val) {
        assertMainThread();
        if (isVmRunning_.load())
            return false;
        size_t fc = getFrameCount();
        if (fc > 0) {
            bool ok = useRegister_ ? regVm_.setFrameLocalAt(fc - 1, name, val) : vm_.setFrameLocalAt(fc - 1, name, val);
            if (ok)
                return true;
        }
        return useRegister_ ? regVm_.setGlobalValue(name, val) : vm_.setGlobalValue(name, val);
    }

    /// P2-1 fix: 检查 VM 条件断点求值是否被请求停止。
    /// 供 IdeController 条件求值 lambda 在每次 evaluate 前检查，
    /// 实现快速中止（对齐 Interpreter 路径 evaluateCondition 中的 stopRequested_ 检查）。
    bool isCondStopRequested() const { return vmCondStopRequested_.load(std::memory_order_relaxed); }

    /// #4 fix: 设置条件求值器回调（由 IdeController 注入，使用临时 Interpreter + VM 全局变量求值）
    void setConditionEvaluator(std::function<bool(const std::string&)> evaluator) override {
        vmConditionEvaluator_ = std::move(evaluator);
    }

    // ---- R104 调试器拓展：Logpoint / Function BP / Exception BP ----
    /// R104 Logpoint：设置日志回调（命中 Logpoint 时调用，输出格式化消息）
    void setLogCallback(std::function<void(const std::string&)> cb) override { vmLogCallback_ = std::move(cb); }

    /// R104 Logpoint：设置断点类型（Line / Logpoint）
    void setBreakpointKind(int line, BreakpointKind kind) override {
        if (line <= 0)
            return;
        vmBreakpointKinds_[line] = kind;
        if (kind == BreakpointKind::Line) {
            vmLogpointMessages_.remove(line);
        }
        vmBreakpointHitCounts_.remove(line);
    }

    /// R104 Logpoint：获取断点类型
    BreakpointKind getBreakpointKind(int line) const override {
        auto it = vmBreakpointKinds_.find(line);
        return it != vmBreakpointKinds_.end() ? it.value() : BreakpointKind::Line;
    }

    /// R104 Logpoint：设置 Logpoint 日志消息模板
    void setLogpointMessage(int line, const std::string& msg) override {
        if (line <= 0)
            return;
        vmBreakpointKinds_[line] = BreakpointKind::Logpoint;
        vmLogpointMessages_[line] = msg;
        vmBreakpointHitCounts_.remove(line);
    }

    /// R104 Logpoint：获取 Logpoint 日志消息模板
    std::string getLogpointMessage(int line) const override {
        auto it = vmLogpointMessages_.find(line);
        return it != vmLogpointMessages_.end() ? it.value() : std::string{};
    }

    /// R104 Function BP：添加函数断点
    // P1-4 fix: 内部存储改为 QMap<std::string, FunctionBreakpointInfo>，
    // 与 DebugController 对齐支持条件求值。setFunctionBreakpoint 仅插入默认（无条件）条目，
    // 若已存在则保留原有 condition（对齐 DebugController::setBreakpoint 行为）。
    void setFunctionBreakpoint(const std::string& name) override {
        if (name.empty())
            return;
        if (!vmFunctionBreakpoints_.contains(name)) {
            vmFunctionBreakpoints_.insert(name, FunctionBreakpointInfo(name));
        }
    }
    /// R104 Function BP：移除函数断点
    void removeFunctionBreakpoint(const std::string& name) override {
        vmFunctionBreakpoints_.remove(name);
        vmFunctionBreakpointHitCounts_.remove(name);
    }
    /// R104 Function BP：批量设置函数断点
    /// P1-4 fix: 保留已有条目的 condition（与 DebugController::setFunctionBreakpoints
    /// 未实现批量 condition 保留的语义对齐——这里更安全，避免批量替换丢失条件）。
    void setFunctionBreakpoints(const QSet<std::string>& names) override {
        QMap<std::string, FunctionBreakpointInfo> next;
        for (const auto& name : names) {
            if (name.empty())
                continue;
            auto it = vmFunctionBreakpoints_.constFind(name);
            if (it != vmFunctionBreakpoints_.constEnd()) {
                next.insert(name, it.value()); // 保留 condition
            } else {
                next.insert(name, FunctionBreakpointInfo(name));
            }
        }
        vmFunctionBreakpoints_ = std::move(next);
        // 清理被移除断点的 hitCount
        for (auto it = vmFunctionBreakpointHitCounts_.begin(); it != vmFunctionBreakpointHitCounts_.end();) {
            if (!vmFunctionBreakpoints_.contains(it.key())) {
                it = vmFunctionBreakpointHitCounts_.erase(it);
            } else {
                ++it;
            }
        }
    }
    /// R104 Function BP：查询所有函数断点名
    /// P1-4 fix: 内部存储改为 QMap<...FunctionBreakpointInfo>，返回时提取 keys。
    QSet<std::string> getFunctionBreakpoints() const override {
        QSet<std::string> result;
        for (auto it = vmFunctionBreakpoints_.constBegin(); it != vmFunctionBreakpoints_.constEnd(); ++it) {
            result.insert(it.key());
        }
        return result;
    }
    /// R104 Function BP：函数断点命中次数
    int getFunctionBreakpointHitCount(const std::string& name) const override {
        auto it = vmFunctionBreakpointHitCounts_.find(name);
        return it != vmFunctionBreakpointHitCounts_.end() ? it.value() : 0;
    }
    /// R104 Function BP：查询是否存在指定函数断点
    bool hasFunctionBreakpoint(const std::string& name) const override { return vmFunctionBreakpoints_.contains(name); }

    /// P1-4 fix: 设置函数断点条件表达式（与 DebugController::setFunctionBreakpointCondition 对齐）。
    /// 函数断点不存在时自动创建（对齐 setBreakpointCondition 行为）。
    /// 条件变更时重置对应函数的 hitCount（对齐 DebugController L556-568 语义）。
    void setFunctionBreakpointCondition(const std::string& name, const std::string& condition) override {
        if (name.empty())
            return;
        auto it = vmFunctionBreakpoints_.find(name);
        if (it != vmFunctionBreakpoints_.end()) {
            it.value().condition = condition;
            it.value().hitCount = 0;
        } else {
            FunctionBreakpointInfo info(name);
            info.condition = condition;
            vmFunctionBreakpoints_.insert(name, std::move(info));
        }
        vmFunctionBreakpointHitCounts_.remove(name);
    }
    /// P1-4 fix: 获取函数断点条件表达式（与 DebugController::getFunctionBreakpointCondition 对齐）。
    std::string getFunctionBreakpointCondition(const std::string& name) const override {
        auto it = vmFunctionBreakpoints_.constFind(name);
        return it != vmFunctionBreakpoints_.constEnd() ? it.value().condition : std::string{};
    }

    /// R104 Exception BP：启用/禁用异常断点
    void setExceptionBreakpointEnabled(bool enabled) override {
        vmExceptionBreakpointEnabled_ = enabled;
        if (!enabled)
            vmExceptionBreakpointHitCount_ = 0;
    }
    /// R104 Exception BP：查询异常断点是否启用
    bool isExceptionBreakpointEnabled() const override { return vmExceptionBreakpointEnabled_; }
    /// R104 Exception BP：异常断点命中次数
    int getExceptionBreakpointHitCount() const override { return vmExceptionBreakpointHitCount_; }

    // ---- R161 Watchpoint（数据断点）----
    /// 添加数据断点（监视变量/字段被修改时暂停）
    void setWatchpoint(const WatchpointInfo& wp) override { vmWatchpoints_.append(wp); }
    /// 移除指定变量名的数据断点
    void removeWatchpoint(const std::string& varName, const std::string& fieldName = "") override {
        for (int i = vmWatchpoints_.size() - 1; i >= 0; --i) {
            if (vmWatchpoints_[i].varName == varName &&
                (fieldName.empty() || vmWatchpoints_[i].fieldName == fieldName)) {
                vmWatchpoints_.removeAt(i);
            }
        }
    }
    /// 清除所有数据断点
    void clearWatchpoints() override { vmWatchpoints_.clear(); }
    /// 查询所有数据断点
    const QVector<WatchpointInfo>& getWatchpoints() const { return vmWatchpoints_; }
    /// 是否有数据断点（快速路径判断）
    bool hasWatchpoints() const override { return !vmWatchpoints_.isEmpty(); }

    // ---- R114 可回放执行时间轴：recorder 集成 ----
    /// 启用/禁用执行轨迹录制。启用后每次 stepOnceActive() 后自动采集快照。
    /// @param enabled 是否启用
    /// @param backend 当前后端类型（StackVM / RegisterVM），用于快照溯源
    /// @note recorder 是主动采集，与 stepCallback 禁用机制兼容——
    ///       stepCallback 用于 UI 信号回调（已禁用避免拖慢），recorder 直接调用
    ///       traceRecorder().captureVmStep() 写入环形缓冲区，不影响步进性能。
    ///       调用方在切换后端时需重新设置 backend 参数。
    void setRecordingEnabled(bool enabled, TraceBackend backend) {
        assertMainThread(); // AUDIT-R4 BUG-14
        recordingEnabled_ = enabled;
        recorderBackend_ = backend;
    }
    bool isRecordingEnabled() const { return recordingEnabled_; }
    TraceBackend recorderBackend() const { return recorderBackend_; }

    /// R114 阶段 3：从 TraceSnapshot 回滚 VM 状态（状态回滚入口）。
    /// 仅 VM 路径（StackVM / RegisterVM）支持回滚，Interpreter 路径不支持
    /// （AST 树遍历无 IP 概念，回滚需重新驱动执行，不在本轮范围）。
    /// @param snap 快照（必须为 StackVM 或 RegisterVM 后端，且 FullState 模式采集）
    /// @return true 成功；false 后端不匹配 / 未初始化 / FullState 字段为空
    /// @note 回滚后 isVmRunning_=false，isVmInitialized_=true，可直接调用
    ///       stepByMode(STEP_IN/RUN) 从目标步继续执行。
    ///       recordingEnabled_ 不变（如需继续录制请保持 true）。
    ///       vmLastPausedLine_/vmCrossedLine_/vmCrossedDeeper_ 重置避免影响下次步进。
    bool restoreFromSnapshot(const TraceSnapshot& snap) {
        assertMainThread(); // AUDIT-R4 BUG-14
        if (snap.backend != TraceBackend::StackVM && snap.backend != TraceBackend::RegisterVM) {
            return false; // Interpreter 路径不支持回滚
        }
        if (useRegister_ && snap.backend != TraceBackend::RegisterVM) {
            return false; // 后端不匹配
        }
        if (!useRegister_ && snap.backend != TraceBackend::StackVM) {
            return false; // 后端不匹配
        }
        // 首次回滚：若 VM 未初始化，需先 initExecution（否则 restoreFromSnapshot 拒绝）
        if (!isVmInitialized_) {
            if (!initActiveExecution()) {
                return false;
            }
            isVmInitialized_ = true;
        }
        bool ok = false;
        if (useRegister_) {
            ok = regVm_.restoreFromSnapshot(snap.registerValues, snap.globalsValues, snap.ip, snap.frameCount);
        } else {
            ok = vm_.restoreFromSnapshot(snap.stackValues, snap.globalsValues, snap.ip, snap.frameCount);
        }
        if (ok) {
            // 重置步进状态机标志，避免影响下次步进
            isVmRunning_ = false;
            vmLastPausedLine_ = 0;
            vmCrossedLine_ = false;
            vmCrossedDeeper_ = false;
        }
        return ok;
    }

    /// 拓展二期：回溯调试单步后退（reverse step）。
    /// 依赖 FullState 模式录制：回滚到轨迹中倒数第二个快照并丢弃
    /// 末尾快照，连续调用可持续后退（reverse-continue = 循环 stepBack，
    /// 或经 ReverseDebugTimelinePanel 直接跳任意历史步）。
    /// @return false：快照不足（<2）/后端不匹配/未以 FullState 录制
    bool stepBack() {
        assertMainThread();
        auto& rec = traceRecorder();
        if (rec.size() < 2)
            return false;
        auto snap = rec.stepAt(rec.size() - 2);
        if (!snap)
            return false;
        if (!restoreFromSnapshot(*snap))
            return false;
        // 丢弃"未来"步：支持连续后退，且重新执行的录制不与旧轨迹混杂
        rec.truncateFrom(rec.size() - 1);
        return true;
    }

    // ---- A1 fix: 后端模式切换 ----
    /// 启用/禁用 RegisterVM 后端。true 时所有步进/状态访问转发到 regVm_。
    /// 切换时自动 reset 两个后端，避免遗留状态污染。
    void setUseRegister(bool enabled) {
        assertMainThread(); // AUDIT-R4 BUG-14
        if (useRegister_ == enabled)
            return;
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
    bool isRegisterMode() const {
        assertMainThread(); // AUDIT-R4 BUG-14
        return useRegister_;
    }

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
    void stop() override;
    /// A4 fix: 重置 VM 状态（用于重新开始）
    // B2 fix: 停止 RUN 模式定时器并重置 isVmRunning_，避免 runBatch 在已 resetState 的 VM 上调用
    // currentFrame() 触发 std::abort。原 reset() 遗漏定时器停止 + 状态复位。
    void reset() override {
        assertMainThread(); // AUDIT-R4 BUG-14
        if (vmRunTimer_)
            vmRunTimer_->stop();
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
        // P0-1 fix: reset() 遗漏 vmLastPausedLine_/vmStepStartFrameCount_ 重置。
        // stop() 正确重置了这两个字段，但 reset() 没有。新调试会话通过 prepareRun()
        // 调用 reset() 而非 stop()，残留的 vmLastPausedLine_ 会污染断点去重逻辑
        // （currentLine != vmLastPausedLine_ 判断失效），首行断点可能被错误过滤。
        vmLastPausedLine_ = 0;
        vmStepStartFrameCount_ = 0;
        // P2-1 fix: 清除条件求值停止标志（新会话开始）
        vmCondStopRequested_.store(false, std::memory_order_relaxed);
        // BUG-DBG-AUDIT-2 fix: 清空断点命中计数（对齐 DebugController::reset 行 485，
        // 重置所有断点 hitCount，保留断点和条件本身）。
        vmBreakpointHitCounts_.clear();
        // R98 runToCursor: reset() 清除临时断点。新调试会话不应继承上一次会话的
        // runToCursor 目标行，否则首次 RUN 模式会在目标行意外暂停。
        vmTempBreakpointLine_ = -1;
        // R104: 清理新断点存储（保留断点配置本身，仅清理运行时状态）
        vmFunctionBreakpointHitCounts_.clear();
        vmExceptionBreakpointHitCount_ = 0;
    }

    bool isRunning() const { return isVmRunning_; }
    bool isInitialized() const {
        assertMainThread(); // AUDIT-R4 BUG-14
        return isVmInitialized_;
    }

    // ---- VM 状态访问器（B6 fix: 语义化快照接口，GUI 仅通过这些方法读取 VM 状态）----
    // B6 bug fix: getStack 改返回 by value，避免返回 vm_.stack_ 引用导致调用方
    // 缓存引用后步进 VM 触发悬垂/use-after-free
    // A1 fix: 栈式 VM 返回操作数栈；RegisterVM 返回寄存器窗口（同样以 vector<Value> 形式）
    std::vector<Value> getStack() const { return useRegister_ ? regVm_.getRegisters() : vm_.getStack(); }
    std::unordered_map<std::string, Value> getGlobals() const {
        return useRegister_ ? regVm_.getGlobals() : vm_.getGlobalsRef();
    }
    /// BUG-IDE-12 fix: 获取当前帧的局部变量名→值映射（用于 VM 条件断点求值）。
    /// 分派到当前激活后端的 getCurrentFrameLocals()。空帧/主程序帧返回空映射。
    std::unordered_map<std::string, Value> getCurrentFrameLocals() const {
        return useRegister_ ? regVm_.getCurrentFrameLocals() : vm_.getCurrentFrameLocals();
    }
    /// R121 调用栈帧切换：获取指定帧索引的局部变量名→值映射。
    /// @param frameIndex 帧索引（0=栈底 main，递增到栈顶；越界返回空映射）
    /// @note 用于 VariableInspector/WatchPanel 在用户切换帧时获取该帧上下文。
    std::unordered_map<std::string, Value> getFrameLocalsAt(size_t frameIndex) const {
        return useRegister_ ? regVm_.getFrameLocalsAt(frameIndex) : vm_.getFrameLocalsAt(frameIndex);
    }
    size_t getCurrentIP() const { return useRegister_ ? regVm_.getCurrentIP() : vm_.getCurrentIP(); }
    // A1 fix: 统一返回操作码名称字符串，兼容 OpCode/RegOp
    std::string getCurrentOpCodeName() const {
        return useRegister_ ? std::string(regOpName(regVm_.getCurrentOpCode()))
                            : std::string(opCodeName(vm_.getCurrentOpCode()));
    }
    int getCurrentLine() const { return useRegister_ ? regVm_.getCurrentLine() : vm_.getCurrentLine(); }
    std::string getCurrentChunkName() const {
        return useRegister_ ? regVm_.getCurrentChunkName() : vm_.getCurrentChunkName();
    }
    std::string getLastError() const { return useRegister_ ? regVm_.getLastError() : vm_.getLastError(); }
    int getLastErrorLine() const { return useRegister_ ? regVm_.getLastErrorLine() : vm_.getLastErrorLine(); }
    // A4 fix: 暴露 VM 调用栈深度（用于 step-over/out 判断）
    size_t getFrameCount() const { return useRegister_ ? regVm_.getFrameCount() : vm_.getFrameCount(); }
    // BUG-DBG-6 fix: 暴露 VM 调用栈快照（用于 GUI 调用栈面板显示）。
    // 原实现 VmStepper 未转发 VM::getCallStack()/RegisterVM::getCallStack()，
    // 导致 VM 模式调试时 DebugPanel 调用栈列表永远空白（数据源是 Interpreter 的空 callStack_）。
    // 转换 VMCallStackEntry/RegCallStackEntry → CallStackEntry：
    //   functionName → functionName
    //   line → line
    //   depth → 帧索引（0=栈底 main，递增到栈顶）
    //   locals → P2-3 fix: 通过 getFrameLocalsAt() 反查各帧局部变量
    std::vector<CallStackEntry> getCallStack() const override {
        std::vector<CallStackEntry> result;
        if (useRegister_) {
            auto frames = regVm_.getCallStack();
            result.reserve(frames.size());
            int depth = 0;
            for (const auto& f : frames) {
                CallStackEntry entry;
                entry.functionName = f.functionName;
                entry.line = f.line;
                entry.depth = depth;
                // P2-3 fix: 通过 getFrameLocalsAt 反查帧局部变量
                auto locals = regVm_.getFrameLocalsAt(static_cast<size_t>(depth));
                for (const auto& kv : locals) {
                    entry.locals.emplace_back(kv.first, kv.second);
                }
                ++depth;
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
                entry.depth = depth;
                // P2-3 fix: 通过 getFrameLocalsAt 反查帧局部变量
                auto locals = vm_.getFrameLocalsAt(static_cast<size_t>(depth));
                for (const auto& kv : locals) {
                    entry.locals.emplace_back(kv.first, kv.second);
                }
                ++depth;
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
    // AUDIT-R4 BUG-14 fix: 线程亲和性断言。VmStepper 的非 atomic 状态字段
    // （isVmInitialized_/useRegister_/vmTempBreakpointLine_ 等）依赖"仅主线程
    // 访问（QTimer + UI 槽）"的约定保证安全。历史上 isVmRunning_ 曾因被 REPL
    // 异步任务跨线程读取而不得不 atomic 化（IDE-ATOMIC-01）——本断言在 Debug
    // 构建中把同类违规从"无提示数据竞争"提前为确定性崩溃，强制约定。
    // Release 构建中 Q_ASSERT 为 no-op，零开销。
    void assertMainThread() const { Q_ASSERT(QThread::currentThread() == thread()); }

    // A1 fix: 双后端实例。RegisterVM 在 useRegister_=false 时闲置，
    // 不持有运行时资源（resetState 后 frames_/globals_ 均空），内存开销可忽略。
    VM vm_;
    RegisterVM regVm_;
    // P1-6 fix: 原 const T* 裸指针，编译器状态变更/移动可能悬垂。改为按值拷贝持有。
    std::optional<CompileResult> lastCompileResult_;
    std::optional<RegisterCompileResult> lastRegCompileResult_; // A1 fix

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
    size_t vmStepStartFrameCount_ = 0; // step-over/out 起始帧深度
    int vmLastPausedLine_ = 0;         // 上次暂停的行号（防同行重复触发）
    // AUDIT-BUG-D2 fix: STEP_OVER 期间是否进入过更深的帧。
    // 与 Interpreter DebugController::crossedDeeper_ 对齐——
    // 同行函数调用返回后即使行号不变也应暂停。
    bool vmCrossedDeeper_ = false;
    // BUG-DBG-2 fix: crossedLine 机制，与 DebugController::crossedLine_ 对齐。
    // 单行循环断点（如 for (...; ...; ...) print(i);）在 RUN 模式下需每次迭代重新触发。
    // 原实现仅用 currentLine != vmLastPausedLine_ 去重，导致首次命中后永不再触发。
    int vmLastSeenLine_ = -1;    // 上次见到的行号
    bool vmCrossedLine_ = false; // 是否跨过不同行（允许同行断点重新触发）
    QSet<int> vmBreakpoints_;    // VM 模式断点行号集合（复用 Editor 断点）
    // R98 runToCursor: 一次性临时断点（与 vmBreakpoints_ 独立存储）。
    // 仅在主线程访问（QTimer + UI 槽），无需 atomic/mutex。
    // -1 表示无临时断点；>0 为目标行号。命中后立即清除（stepByMode/runBatch 中处理）。
    int vmTempBreakpointLine_ = -1;
    QMap<int, std::string> vmBreakpointConditions_; // #4 fix: 条件断点表达式
    // 拓展二期：命中条件（行号→表达式）与依赖断点链（行号→依赖行）。
    // 判定集中在 breakpointGateAllows（RUN 模式断点命中即将暂停前的统一门控）。
    QMap<int, std::string> vmBreakpointHitConditions_;
    QMap<int, int> vmBreakpointDependencies_;
    // BUG-DBG-AUDIT-2 fix: VM 模式断点命中计数（行号→次数），对齐
    // DebugController::breakpointInfos_[line].hitCount。checkBreakpointHit 命中时递增，
    // reset() 清空，setBreakpointConditions 重置对应行。
    QMap<int, int> vmBreakpointHitCounts_;
    std::function<bool(const std::string&)> vmConditionEvaluator_; // #4 fix: 条件求值回调
    // P2-1 fix: VM 条件断点求值期间的停止标志。用户点击停止时 stop() 设置此标志，
    // 条件求值 lambda 在每次 evaluate 前检查，实现快速中止（无需等待步数上限）。
    // 对齐 Interpreter 路径 evaluateCondition 中检查 stopRequested_ 的机制。
    std::atomic<bool> vmCondStopRequested_{false};

    // R104 调试器拓展存储
    // Logpoint：断点类型（行号→kind）+ 日志消息模板。命中时不暂停，仅输出日志。
    QMap<int, BreakpointKind> vmBreakpointKinds_;
    QMap<int, std::string> vmLogpointMessages_;
    std::function<void(const std::string&)> vmLogCallback_;
    // Function BP：函数名→条件信息映射。pre-execution 检测 OP_CALL/REG_CALL 时匹配。
    // P1-4 fix: 从 QSet<std::string> 改为 QMap<std::string, FunctionBreakpointInfo>，
    // 与 DebugController::functionBreakpoints_ 对齐，支持条件求值（R104 移植 R161 一致性）。
    QMap<std::string, FunctionBreakpointInfo> vmFunctionBreakpoints_;
    QMap<std::string, int> vmFunctionBreakpointHitCounts_;
    // Exception BP：单一全局开关。pre-execution 检测 OP_THROW/REG_THROW 时暂停。
    bool vmExceptionBreakpointEnabled_ = false;
    int vmExceptionBreakpointHitCount_ = 0;
    // R161 Watchpoint：数据断点列表。pre-execution 检测 SET 类指令时匹配。
    QVector<WatchpointInfo> vmWatchpoints_;
    // QT-R-01 fix: RUN 模式异步分批执行的定时器
    QTimer* vmRunTimer_ = nullptr;
    int64_t vmRunStepCount_ = 0; // RUN 模式累计执行步数（用于总量上限保护）

    // ---- R114 可回放执行时间轴：recorder 状态 ----
    // recordingEnabled_ 为 true 时，execStepIn/Over/Out/runBatch 在每次
    // stepOnceActive() 后调用 traceRecorder().captureVmStep(*this, recorderBackend_)。
    // 与 stepCallback 禁用机制兼容——recorder 是主动采集，不依赖信号回调。
    bool recordingEnabled_ = false;
    TraceBackend recorderBackend_ = TraceBackend::StackVM;

    // ---- A1 fix: 单步执行分派辅助 ----
    // 两个 VM 的 stepOnce 均返回 VMResult，无需 vtable，直接 if 分派更高效
    VMResult stepOnceActive();
    bool isActiveFinished() const;
    bool initActiveExecution(); // P1-5 fix: 返回 false 表示编译结果为空，caller 不应标记 initialized
    void resetActiveState();

    /// #4 fix: 检查断点命中（含条件求值）。返回 true 表示应在此行暂停。
    /// line: 当前 IP 所在行号。若该行有条件断点，调用 evaluator 求值；
    /// 无条件或求值为真时返回 true。
    /// R104 Logpoint：命中时输出日志并递增 hitCount，但返回 false（不暂停）。
    bool checkBreakpointHit(int line);

    /// 拓展二期：断点暂停前的统一门控。在既有命中判定
    /// （checkBreakpointHit + lastPaused/crossedLine 门控）全部通过、
    /// 即将暂停前调用：
    ///   1. 依赖链：依赖行断点从未命中过 → false（未激活，不计数不暂停）
    ///   2. 递增 hitCount（到达即计数，原调用点的递增已内联至此）
    ///   3. 命中条件不满足 → false（已计数但不暂停）
    /// 返回 false 时调用方不更新 vmLastPausedLine_/vmCrossedLine_，
    /// 下次到达同行仍可重新判定。
    bool breakpointGateAllows(int line);

    /// R104 Logpoint：处理 Logpoint 命中（输出日志、递增 hitCount）。
    /// @return true 表示已处理（调用方应跳过此断点不暂停）
    bool handleLogpointHit(int line);

    /// R104 Function BP：pre-execution 检测 OP_CALL/REG_CALL 是否命中函数断点。
    /// @return true 表示应暂停
    bool checkFunctionBreakpointHit();

    /// R104 Exception BP：pre-execution 检测 OP_THROW/REG_THROW 是否命中异常断点。
    /// @return true 表示应暂停
    bool checkExceptionBreakpointHit();

    /// R104 辅助：pre-execution 统一检查函数断点 + 异常断点。
    /// @param line 当前 IP 所在行号（用于设置 vmLastPausedLine_）
    /// @return true 表示命中函数或异常断点（已设置 vmLastPausedLine_，调用方应返回 PAUSED_AT_BREAKPOINT）
    bool checkPreExecutionFunctionExceptionBps(int line);

    /// R161 Watchpoint：pre-execution 检查当前指令是否写入被监视的变量/字段。
    /// 通过 peekWriteTarget() 读取当前 IP 指令的写入目标，与 vmWatchpoints_ 匹配。
    /// @param line 当前 IP 所在行号（用于设置 vmLastPausedLine_）
    /// @return true 表示命中数据断点（已设置 vmLastPausedLine_，调用方应返回 PAUSED_AT_BREAKPOINT）
    bool checkWatchpointHit(int line);

    // ---- 步进模式分派（按 VmStepMode 拆分自 stepByMode）----
    // 由 stepByMode 在完成公共前置（编译结果检查 / VM 初始化 / 起始帧深度记录 /
    // crossed 标志重置 / 用户断点 pre-execution 检查 / stepCallback 禁用）后调用。
    // 每个方法独立完成对应模式的步进循环并返回结果状态。
    VmStepResult execStepRun();  // RUN：R98 临时断点 pre-execution 检查 + QTimer 异步分批
    VmStepResult execStepIn();   // STEP_IN：行号或帧深度变化时暂停（R82 P1-2 fix 行级粒度）
    VmStepResult execStepOver(); // STEP_OVER：帧深度回到起始且行号变化时暂停（含 crossedDeeper）
    VmStepResult execStepOut();  // STEP_OUT：帧深度比起始更浅时暂停（栈底降级为跨行暂停）

    /// R114 可回放执行时间轴：条件性采集快照。
    /// recordingEnabled_ 为 true 时调用 traceRecorder().captureVmStep()。
    /// 由 execStepIn/Over/Out/runBatch 在每次 stepOnceActive() 后调用。
    /// 放在 private 头部以便四个 execXxx 复用。
    void maybeRecordStep() {
        if (recordingEnabled_) {
            traceRecorder().captureVmStep(*this, recorderBackend_);
        }
    }
};
